// Pre-js for the Node.js test build (LIBERTY_WEB_NODERAWFS=ON).
// Emscripten does not expose the host environment to getenv(); forward the
// variables that choose where the title keeps its data (XDG_DATA_HOME/HOME,
// see rex::filesystem) so a run can point at a local game directory.
if (ENVIRONMENT_IS_NODE) {
  Module['preRun'] = [].concat(Module['preRun'] || [], () => {
    for (const name of ['XDG_DATA_HOME', 'HOME']) {
      if (process.env[name]) ENV[name] = process.env[name];
    }
  });
}
