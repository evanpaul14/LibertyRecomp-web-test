// Pipeline recipes: the keys of the title pipelines earlier runs created,
// compiled ahead of their first draw. A browser compiles a new pipeline in
// 1.5-5 s while its shader cache is cold; meanwhile the draws using it are
// skipped and the guest audio mixer falls behind.
//
// In the browser the recipes live in IndexedDB (database liberty-webgpu,
// store recipes, key "pipelines"), and a seed file served next to the page
// (--webgpu_pipeline_seed_url) is merged in. Node test builds keep them in
// --webgpu_pipeline_recipe_file, which is also how a seed is recorded.
//
// File format (little endian): u32 magic 'LRPR', u32 version, u32 count, then
// per recipe u32 word count and the words of its key (DrawPipeline's layout).
// Several blobs may follow one another (the stored list, then the seed).

#include "renderer_state.h"

#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(webgpu_pipeline_recipes, true, "GPU",
                    "Web build: remember title pipelines between runs and compile them ahead "
                    "of their first draw");
REXCVAR_DEFINE_STRING(webgpu_pipeline_recipe_file, "", "GPU",
                      "Web build: keep pipeline recipes in this file instead of the browser's "
                      "IndexedDB (Node test builds; also records a seed for the browser)");
REXCVAR_DEFINE_STRING(webgpu_pipeline_seed_url, "pipeline_seed.bin", "GPU",
                      "Web build: recipes fetched from this URL (relative to the page; ignored "
                      "when missing) and compiled ahead along with the browser's own");
REXCVAR_DEFINE_UINT32(webgpu_pipeline_recipes_in_flight, 4, "GPU",
                      "Web build: recipes compiling at once (more delays the pipelines that "
                      "draws are waiting for)");

namespace rex::graphics::gta4_webgpu {
namespace {

constexpr uint32_t kRecipeMagic = 0x5250524C;  // "LRPR"
// Bump when DrawPipeline's key layout changes: older recipes are dropped.
constexpr uint32_t kRecipeVersion = 1;
// Below the pipeline cache's limit (DrawPipeline clears it at 8192).
constexpr size_t kMaximumRecipes = 4096;
constexpr uint32_t kMaximumRecipeWords = 1024;
constexpr double kSaveIntervalMs = 10000;

// Starts loading the stored list and the seed; liberty_recipes_size reports
// -1 until both have answered (a missing one counts as empty).
EM_JS(void, liberty_recipes_load, (const char* seed_url), {
  var url = UTF8ToString(Number(seed_url));
  var state = {store: null, seed: null, pending: 2, db: null};
  globalThis.libertyRecipes = state;
  var answered = () => { state.pending--; };
  try {
    if (typeof indexedDB == 'undefined') throw 0;
    var open = indexedDB.open('liberty-webgpu', 1);
    open.onupgradeneeded = () => open.result.createObjectStore('recipes');
    open.onerror = answered;
    open.onsuccess = () => {
      state.db = open.result;
      try {
        var get = state.db.transaction('recipes').objectStore('recipes').get('pipelines');
        get.onsuccess = () => {
          if (get.result instanceof Uint8Array) state.store = get.result;
          answered();
        };
        get.onerror = answered;
      } catch (e) { answered(); }
    };
  } catch (e) { answered(); }
  if (!url || typeof fetch == 'undefined' || typeof location == 'undefined') {
    answered();
  } else {
    fetch(url, {cache: 'no-cache'})
        .then((response) => response.ok ? response.arrayBuffer() : null)
        .then((buffer) => { if (buffer) state.seed = new Uint8Array(buffer); },
              () => {})
        .finally(answered);
  }
});

EM_JS(double, liberty_recipes_size, (), {
  var state = globalThis.libertyRecipes;
  if (!state || state.pending > 0) return -1;
  return (state.store ? state.store.length : 0) + (state.seed ? state.seed.length : 0);
});

// Copies the stored list and then the seed to `out`.
EM_JS(void, liberty_recipes_take, (uint8_t* out), {
  var state = globalThis.libertyRecipes;
  var at = Number(out);
  if (state.store) { HEAPU8.set(state.store, at); at += state.store.length; }
  if (state.seed) HEAPU8.set(state.seed, at);
  state.store = state.seed = null;
});

EM_JS(void, liberty_recipes_store, (const uint8_t* bytes, double size), {
  var state = globalThis.libertyRecipes;
  if (!state || !state.db) return;
  var start = Number(bytes);
  // slice copies out of the shared heap (IndexedDB cannot store a view of it).
  var copy = HEAPU8.slice(start, start + size);
  try {
    state.db.transaction('recipes', 'readwrite').objectStore('recipes').put(copy, 'pipelines');
  } catch (e) {}
});

uint32_t ReadWord(std::span<const uint8_t> bytes, size_t at) {
  uint32_t value;
  std::memcpy(&value, bytes.data() + at, sizeof(value));
  return value;
}

void AppendWord(std::vector<uint8_t>& out, uint32_t value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(value));
}

}  // namespace

void Renderer::State::LoadRecipes() {
  if (!REXCVAR_GET(webgpu_pipeline_recipes)) return;
  const std::string& path = REXCVAR_GET(webgpu_pipeline_recipe_file);
  if (path.empty()) {
    liberty_recipes_load(REXCVAR_GET(webgpu_pipeline_seed_url).c_str());
    recipes_loading = true;
    return;
  }
  std::vector<uint8_t> bytes;
  if (FILE* file = std::fopen(path.c_str(), "rb")) {
    uint8_t buffer[65536];
    size_t read;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
      bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
  }
  ParseRecipes(bytes);
}

void Renderer::State::ParseRecipes(std::span<const uint8_t> bytes) {
  size_t at = 0, added = 0, dropped = 0;
  while (at + 12 <= bytes.size()) {
    const uint32_t magic = ReadWord(bytes, at), version = ReadWord(bytes, at + 4);
    const uint32_t count = ReadWord(bytes, at + 8);
    at += 12;
    if (magic != kRecipeMagic) break;
    for (uint32_t i = 0; i < count && at + 4 <= bytes.size(); ++i) {
      const uint32_t words = ReadWord(bytes, at);
      at += 4;
      if (words > kMaximumRecipeWords || at + size_t(words) * 4 > bytes.size()) {
        at = bytes.size();
        break;
      }
      Words key(words);
      std::memcpy(key.data(), bytes.data() + at, size_t(words) * 4);
      at += size_t(words) * 4;
      if (version != kRecipeVersion) {
        ++dropped;
        continue;
      }
      if (recipes_known.size() >= kMaximumRecipes) continue;
      auto [entry, inserted] = recipes_known.emplace(std::move(key), uint32_t(recipe_order.size()));
      if (!inserted) continue;
      recipe_order.push_back(&entry->first);
      recipe_queue.push_back(entry->first);
      ++added;
    }
  }
  REXLOG_INFO("gta4-webgpu: {} pipeline recipes to compile ahead ({} bytes, {} of an older "
              "version dropped)",
              added, bytes.size(), dropped);
  // Recipes of an older version would stay stored otherwise.
  if (dropped) ++recipes_unsaved;
}

void Renderer::State::PrecompileRecipes(double deadline) {
  if (recipes_loading) {
    const double size = liberty_recipes_size();
    if (size < 0) return;
    recipes_loading = false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty()) liberty_recipes_take(bytes.data());
    ParseRecipes(bytes);
  }
  std::string error;
  while (!recipe_queue.empty() &&
         recipes_in_flight < REXCVAR_GET(webgpu_pipeline_recipes_in_flight) &&
         emscripten_get_now() < deadline) {
    const Words key = std::move(recipe_queue.front());
    recipe_queue.pop_front();
    if (key.size() < 4 || pipelines.contains(key)) continue;
    const uint64_t vertex_hash = key[0] | uint64_t(key[1]) << 32;
    const uint64_t pixel_hash = key[2] | uint64_t(key[3]) << 32;
    const auto* vertex = archive->Find(vertex_hash, gta4_native::ShaderStage::kVertex);
    const auto* pixel = pixel_hash ? archive->Find(pixel_hash, gta4_native::ShaderStage::kPixel) : nullptr;
    if (!vertex || (pixel_hash && !pixel)) {
      ++timing.recipes_failed;
      continue;
    }
    error.clear();
    if (!CreatePipeline(key, *vertex, pixel, true, error)) {
      ++timing.recipes_failed;
      if (timing.recipes_failed <= 8)
        REXLOG_WARN("gta4-webgpu: pipeline recipe {:016X}/{:016X} skipped: {}", vertex_hash,
                    pixel_hash, error);
    }
  }
}

void Renderer::State::RecordRecipe(const Words& key) {
  if (!REXCVAR_GET(webgpu_pipeline_recipes) || recipes_known.size() >= kMaximumRecipes) return;
  auto [entry, inserted] = recipes_known.emplace(key, uint32_t(recipe_order.size()));
  if (!inserted) return;
  recipe_order.push_back(&entry->first);
  ++recipes_unsaved;
}

void Renderer::State::SaveRecipes(bool now) {
  // While loading, a save would replace the stored list with a partial one.
  if (!recipes_unsaved || recipes_loading || !REXCVAR_GET(webgpu_pipeline_recipes)) return;
  const double time = emscripten_get_now();
  if (!now && time - recipes_saved_ms < kSaveIntervalMs) return;
  recipes_saved_ms = time;
  recipes_unsaved = 0;
  std::vector<uint8_t> bytes;
  AppendWord(bytes, kRecipeMagic);
  AppendWord(bytes, kRecipeVersion);
  AppendWord(bytes, uint32_t(recipe_order.size()));
  for (const Words* key : recipe_order) {
    AppendWord(bytes, uint32_t(key->size()));
    for (uint32_t word : *key) AppendWord(bytes, word);
  }
  const std::string& path = REXCVAR_GET(webgpu_pipeline_recipe_file);
  if (path.empty()) {
    liberty_recipes_store(bytes.data(), double(bytes.size()));
    return;
  }
  const std::string temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) {
    REXLOG_WARN("gta4-webgpu: cannot write pipeline recipes to {}", temporary);
    return;
  }
  const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  if (std::fclose(file) != 0 || !written || std::rename(temporary.c_str(), path.c_str()) != 0)
    REXLOG_WARN("gta4-webgpu: cannot write pipeline recipes to {}", path);
}

}  // namespace rex::graphics::gta4_webgpu
