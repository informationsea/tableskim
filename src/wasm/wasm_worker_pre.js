// The worker side of tableskim-wasm and tableskim-wasm-node (see
// wasm_api.cxx). The module is the worker's own script: Emscripten starts
// its threads from that script. In a browser the worker is a Web Worker;
// in Node.js (the VS Code extension, vscode/) a worker_threads Worker.
//
// The page posts requests {id, method, params} and gets back
//   {type: 'ready'}                  once requests can be answered,
//   {type: 'reply', json}            the reply to a request, as JSON text,
//   {type: 'events', json}           {sheet id: WebSession::Event bits},
//   {type: 'file', id, data}         the answer to "readFile".
// In a browser an "open" request carries the File itself (params.file): it
// is mounted here, read in place by WORKERFS. In Node.js it carries the
// file's path on the host (params.path), whose directory is mounted with
// NODEFS. Either way the session gets the mounted path.
// "readFile" ({path}, a file the session wrote, such as an export) is
// answered here, with the file's bytes as a transferred ArrayBuffer.
if ((ENVIRONMENT_IS_WORKER || ENVIRONMENT_IS_NODE) && !ENVIRONMENT_IS_PTHREAD)
{
    var tskStarted = false;
    var tskWaiting = [];
    var tskMounts = 0;
    var tskOpenMounts = {}; // request id -> mount directory
    var tskCloses = {}; // ids of "close" requests not answered yet
    var tskPort = ENVIRONMENT_IS_NODE ? require('node:worker_threads').parentPort : null;

    var tskPostToPage = function (message, transfer)
    {
        if (tskPort)
            tskPort.postMessage(message, transfer);
        else
            postMessage(message, transfer);
    };

    // The directory to mount for a path on the host, and the file's name in it.
    var tskHostPath = function (path)
    {
        var nodePath = require('node:path');
        return { dir: nodePath.dirname(path), name: nodePath.basename(path) };
    };

    var tskSend = function (message)
    {
        var params = message.params || {};
        if (message.method === 'readFile')
        {
            try
            {
                var data = FS.readFile(params.path);
                tskPostToPage({ type: 'file', id: message.id, data: data.buffer }, [data.buffer]);
            }
            catch (e)
            {
                tskPostToPage({ type: 'reply', json: JSON.stringify({ id: message.id, error: 'cannot read ' + params.path }) });
            }
            return;
        }
        if (message.method === 'open' && (params.file || (ENVIRONMENT_IS_NODE && params.path)))
        {
            var dir = '/input/' + (++tskMounts);
            FS.mkdirTree(dir);
            var name;
            if (params.file)
            {
                FS.mount(WORKERFS, { files: [params.file] }, dir);
                name = params.file.name;
            }
            else
            {
                var host = tskHostPath(params.path);
                FS.mount(NODEFS, { root: host.dir }, dir);
                name = host.name;
            }
            tskOpenMounts[message.id] = dir;
            params = Object.assign({}, params, { path: dir + '/' + name });
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
        tskPostToPage({ type: 'reply', json: json });
    };
    Module['tskEvents'] = function (json)
    {
        tskPostToPage({ type: 'events', json: json });
    };
    Module['tskReady'] = function ()
    {
        tskStarted = true;
        tskWaiting.splice(0).forEach(tskSend);
        tskPostToPage({ type: 'ready' });
    };
    Module['print'] = function (text) { console.log(text); };
    Module['printErr'] = function (text) { console.warn(text); };

    // Requests may come before the module is instantiated.
    var tskReceive = function (data)
    {
        if (tskStarted)
            tskSend(data);
        else
            tskWaiting.push(data);
    };
    if (tskPort)
        tskPort.on('message', tskReceive);
    else
        self.onmessage = function (event) { tskReceive(event.data); };
}
