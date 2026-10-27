// The worker side of tableskim-wasm (see wasm_api.cxx). The module is the
// worker's own script: Emscripten starts its threads from that script.
//
// The page posts requests {id, method, params} and gets back
//   {type: 'ready'}                  once requests can be answered,
//   {type: 'reply', json}            the reply to a request, as JSON text,
//   {type: 'events', json}           {sheet id: WebSession::Event bits}.
// An "open" request carries the File itself (params.file): it is mounted
// here, read in place by WORKERFS, and the session gets its path.
if (ENVIRONMENT_IS_WORKER && !ENVIRONMENT_IS_PTHREAD)
{
    var tskStarted = false;
    var tskWaiting = [];
    var tskMounts = 0;
    var tskOpenMounts = {}; // request id -> mount directory

    var tskSend = function (message)
    {
        var params = message.params || {};
        if (message.method === 'open' && params.file)
        {
            var dir = '/input/' + (++tskMounts);
            FS.mkdirTree(dir);
            FS.mount(WORKERFS, { files: [params.file] }, dir);
            tskOpenMounts[message.id] = dir;
            params = Object.assign({}, params, { path: dir + '/' + params.file.name });
            delete params.file;
        }
        _tskPost(stringToNewUTF8(JSON.stringify({ id: message.id, method: message.method, params: params })));
    };

    // The session closes the book before opening the next file, so once an
    // "open" is answered, the files mounted for earlier ones are unused.
    // Later ones may not have been opened yet (files opened in quick
    // succession): they stay.
    var tskReleaseMounts = function (json)
    {
        var match = /^\{"id":(\d+)/.exec(json);
        var id = match && Number(match[1]);
        if (!tskOpenMounts[id])
            return;
        for (var key in tskOpenMounts)
        {
            var dir = tskOpenMounts[key];
            if (Number(key) >= id)
                continue;
            try
            {
                FS.unmount(dir);
                FS.rmdir(dir);
            }
            catch (e)
            {
                // Already gone.
            }
            delete tskOpenMounts[key];
        }
    };

    Module['tskReply'] = function (json)
    {
        tskReleaseMounts(json);
        postMessage({ type: 'reply', json: json });
    };
    Module['tskEvents'] = function (json)
    {
        postMessage({ type: 'events', json: json });
    };
    Module['tskReady'] = function ()
    {
        tskStarted = true;
        tskWaiting.splice(0).forEach(tskSend);
        postMessage({ type: 'ready' });
    };
    Module['print'] = function (text) { console.log(text); };
    Module['printErr'] = function (text) { console.warn(text); };

    // Requests may come before the module is instantiated.
    self.onmessage = function (event)
    {
        if (tskStarted)
            tskSend(event.data);
        else
            tskWaiting.push(event.data);
    };
}
