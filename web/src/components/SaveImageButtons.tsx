import { type RefObject, useState } from 'react';
import { type ChartImage, type ImageFormat, saveChart } from '../charts/image';
import { errorText } from '../hooks';

interface SaveImageButtonsProps {
    chart: RefObject<ChartImage | null>;
    name: string; // of the file, without the extension
    enabled: boolean;
    onError: (message: string) => void;
}

/** Saves the chart as shown, as PNG or SVG, as the GUI's Save Image... does. */
export function SaveImageButtons({ chart, name, enabled, onError }: SaveImageButtonsProps) {
    const [saving, setSaving] = useState(false);
    const save = async (format: ImageFormat) => {
        const image = chart.current?.image();
        if (!image) return;
        setSaving(true);
        try {
            await saveChart(image.svg, image.width, image.height, format, name);
        } catch (failure) {
            onError(`Cannot save the image: ${errorText(failure)}`);
        } finally {
            setSaving(false);
        }
    };
    return (
        <span className="flex items-center gap-1">
            {(['png', 'svg'] as const).map((format) => (
                <button
                    key={format}
                    type="button"
                    id={`save-${format}`}
                    className="control"
                    title={`Save the chart as ${format.toUpperCase()}`}
                    disabled={!enabled || saving}
                    onClick={() => void save(format)}
                >
                    Save {format.toUpperCase()}
                </button>
            ))}
        </span>
    );
}
