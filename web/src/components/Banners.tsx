interface CheckpointBannerProps
{
    checkpoint: { bytes: number; rows: number } | null;
    onContinue: () => void;
    onStop: () => void;
}

function mebibytes(bytes: number)
{
    return `${Math.round(bytes / 1048576).toLocaleString()} MiB`;
}

export function CheckpointBanner({ checkpoint, onContinue, onStop }: CheckpointBannerProps)
{
    return (
        <div id="checkpoint" className="flex flex-wrap items-center gap-2 border-b border-line bg-banner px-2.5 py-1.5"
             hidden={!checkpoint}>
            {checkpoint && (
                <span id="checkpoint-text">
                    Reading paused after {checkpoint.rows.toLocaleString()} rows ({mebibytes(checkpoint.bytes)} in memory).
                </span>
            )}
            <button type="button" id="continue-reading" className="control" onClick={onContinue}>Continue reading</button>
            <button type="button" id="stop-reading" className="control" onClick={onStop}>Stop here</button>
        </div>
    );
}

export function MessageBanner({ message }: { message: string })
{
    return (
        <div id="message" role="alert" className="border-b border-line bg-danger-bg px-2.5 py-1.5 text-danger" hidden={!message}>
            {message}
        </div>
    );
}
