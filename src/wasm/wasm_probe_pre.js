// Mounts the files the probe reads before main() runs: the host file system
// under /host in Node.js, and in a browser the File that the page posts to
// this script's worker, under /input, read in place by WORKERFS. The
// module must be the worker's own script, since its threads load the
// script of the worker that starts them.
Module['preRun'] = Module['preRun'] || [];
if (ENVIRONMENT_IS_NODE)
{
    Module['preRun'].push(function ()
    {
        FS.mkdir('/host');
        FS.mount(NODEFS, { root: '/' }, '/host');
    });
}
else if (ENVIRONMENT_IS_WORKER && !ENVIRONMENT_IS_PTHREAD)
{
    // Filled in below, once the page has posted the file. (In Node.js the
    // arguments come from the command line.)
    Module['arguments'] = [];
    var post = function (message) { postMessage(message); };
    Module['print'] = Module['printErr'] = function (text) { post({ type: 'print', text: text }); };
    Module['onExit'] = function (code) { post({ type: 'exit', code: code }); };

    // The page may post before the module is instantiated: keep the input
    // until preRun, which waits for it if need be.
    var probeInput = null;
    var probeInputArrived = null;
    self.onmessage = function (event)
    {
        probeInput = event.data;
        if (probeInputArrived)
            probeInputArrived();
    };
    Module['preRun'].push(function ()
    {
        var useInput = function ()
        {
            FS.mkdir('/input');
            FS.mount(WORKERFS, { files: [probeInput.file] }, '/input');
            Module['arguments'].push('/input/' + probeInput.file.name);
            if (probeInput.sql)
                Module['arguments'].push(probeInput.sql);
        };
        if (probeInput)
            return useInput();
        addRunDependency('probe-input');
        probeInputArrived = function ()
        {
            useInput();
            removeRunDependency('probe-input');
        };
    });
}
