// The worker side of tableskim-wasm (see wasm_api.cxx). The module is the
// worker's own script: Emscripten starts its threads from that script.
//
// The page posts requests {id, method, params} and gets back
//   {type: 'ready'}                  once requests can be answered,
//   {type: 'reply', json}            the reply to a request, as JSON text,
//   {type: 'events', json}           {sheet id: WebSession::Event bits},
//   {type: 'file', id, data}         the answer to "readFile".
// An "open" request carries the File itself (params.file): it is mounted
// here, read in place by WORKERFS, and the session gets its path.
// "readFile" ({path}, a file the session wrote, such as an export) is
// answered here, with the file's bytes as a transferred ArrayBuffer.
if (ENVIRONMENT_IS_WORKER && !ENVIRONMENT_IS_PTHREAD)
{
    var tskStarted = false;
    var tskWaiting = [];
    var tskMounts = 0;
    var tskOpenMounts = {}; // request id -> mount directory
    var tskCloses = {}; // ids of "close" requests not answered yet

    var tskSend = function (message)
    {
        var params = message.params || {};
        if (message.method === 'readFile')
        {
            try
            {
                var data = FS.readFile(params.path);
                postMessage({ type: 'file', id: message.id, data: data.buffer }, [data.buffer]);
            }
            catch (e)
            {
                postMessage({ type: 'reply', json: JSON.stringify({ id: message.id, error: 'cannot read ' + params.path }) });
            }
            return;
        }
        if (message.method === 'open' && params.file)
        {
            var dir = '/input/' + (++tskMounts);
            FS.mkdirTree(dir);
            FS.mount(WORKERFS, { files: [params.file] }, dir);
            tskOpenMounts[message.id] = dir;
            params = Object.assign({}, params, { path: dir + '/' + params.file.name });
            delete params.file;
        }
        else if (message.method === 'close')
            tskCloses[message.id] = true;
        _tskPost(stringToNewUTF8(JSON.stringify({ id: message.id, method: message.method, params: params })));
    };

    var tskUnmount = function (key)
    {
        try
        {
            FS.unmount(tskOpenMounts[key]);
            FS.rmdir(tskOpenMounts[key]);
        }
        catch (e)
        {
            // Already gone.
        }
        delete tskOpenMounts[key];
    };

    // The session closes the book before opening the next file, so once an
    // "open" or "close" is answered, the files mounted for earlier opens are
    // unused, and so is the open's own file if it failed. Later ones may not
    // have been opened yet (files opened in quick succession): they stay.
    var tskReleaseMounts = function (json)
    {
        var match = /^\{"id":(\d+)(,"error")?/.exec(json);
        var id = match && Number(match[1]);
        if (!tskOpenMounts[id] && !tskCloses[id])
            return;
        delete tskCloses[id];
        for (var key in tskOpenMounts)
        {
            if (Number(key) < id || (Number(key) === id && match[2]))
                tskUnmount(key);
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
