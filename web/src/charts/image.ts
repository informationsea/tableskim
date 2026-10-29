// Saving a chart as an image, as the GUI's Save Image... does: SVG keeps
// lines and text sharp, PNG is the SVG drawn at twice the size.

export type ImageFormat = 'png' | 'svg';

/** What a chart component hands its dialog to save it. */
export interface ChartImage {
    image(): { svg: string; width: number; height: number } | null;
}

const SVG_NS = 'http://www.w3.org/2000/svg';
const PNG_SCALE = 2;

// What the charts' classes set; copied inline, since a saved file has no style sheet.
const STYLE_PROPERTIES = [
    'fill',
    'fill-opacity',
    'stroke',
    'stroke-width',
    'stroke-opacity',
    'opacity',
    'font-family',
    'font-size',
    'font-weight',
    'font-variant-numeric',
] as const;

function escapeXml(text: string) {
    return text.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
}

/** The page's surface colour, which the saved image is drawn on. */
export function surfaceColor(element: Element) {
    return getComputedStyle(element).getPropertyValue('--surface').trim() || '#ffffff';
}

/** A chart drawn as SVG on the page, standalone: its styles inlined, on the surface colour. */
export function svgOfElement(svg: SVGSVGElement): string {
    const copy = svg.cloneNode(true) as SVGSVGElement;
    const sources = [svg, ...svg.querySelectorAll('*')];
    const targets = [copy, ...copy.querySelectorAll('*')];
    sources.forEach((source, i) => {
        const target = targets[i];
        const style = getComputedStyle(source);
        for (const property of STYLE_PROPERTIES) {
            const value = style.getPropertyValue(property);
            if (value) target.setAttribute(property, value);
        }
        target.removeAttribute('class');
        target.removeAttribute('tabindex');
    });
    // Transparent hit areas stay transparent; nothing else answers the pointer in a file.
    copy.setAttribute('xmlns', SVG_NS);
    const width = svg.width.baseVal.value;
    const height = svg.height.baseVal.value;
    copy.setAttribute('viewBox', `0 0 ${width} ${height}`);
    const background = document.createElementNS(SVG_NS, 'rect');
    background.setAttribute('width', '100%');
    background.setAttribute('height', '100%');
    background.setAttribute('fill', surfaceColor(svg));
    copy.insertBefore(background, copy.firstChild);
    return new XMLSerializer().serializeToString(copy);
}

/** The SVG drawn into a PNG at twice its size. */
export async function pngOfSvg(svg: string, width: number, height: number): Promise<Blob> {
    const url = URL.createObjectURL(new Blob([svg], { type: 'image/svg+xml' }));
    try {
        const image = new Image();
        image.src = url;
        await image.decode();
        const canvas = document.createElement('canvas');
        canvas.width = Math.round(width * PNG_SCALE);
        canvas.height = Math.round(height * PNG_SCALE);
        const context = canvas.getContext('2d');
        if (!context) throw new Error('This browser cannot draw the image.');
        context.drawImage(image, 0, 0, canvas.width, canvas.height);
        return await new Promise((resolve, reject) =>
            canvas.toBlob((blob) => (blob ? resolve(blob) : reject(new Error('Cannot make the PNG.'))), 'image/png'),
        );
    } finally {
        URL.revokeObjectURL(url);
    }
}

/** Hands a file to the browser to save. */
export function download(blob: Blob, name: string) {
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = name;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
}

/** Saves a chart given as SVG text in the format asked for. */
export async function saveChart(svg: string, width: number, height: number, format: ImageFormat, name: string) {
    const blob = format === 'svg' ? new Blob([svg], { type: 'image/svg+xml' }) : await pngOfSvg(svg, width, height);
    download(blob, `${fileNamePart(name)}.${format}`);
}

/** A name without the characters file systems reject. */
export function fileNamePart(name: string) {
    const safe = [...name].map((c) => (c < ' ' || '\\/:*?"<>|'.includes(c) ? '_' : c)).join('');
    return safe.trim() || 'chart';
}

/**
 * The part of CanvasRenderingContext2D the scatter plot draws with, recorded
 * as SVG, so that the saved plot is what the canvas shows.
 */
export class SvgContext {
    fillStyle = '#000';
    strokeStyle = '#000';
    lineWidth = 1;
    lineCap: CanvasLineCap = 'butt';
    globalAlpha = 1;
    font = '10px sans-serif';
    textAlign: CanvasTextAlign = 'start';
    textBaseline: CanvasTextBaseline = 'alphabetic';

    private readonly parts: string[] = [];
    private path = '';
    private clips = 0;
    private readonly stack: { state: SvgState; groups: number }[] = [];
    private groups = 0; // clip groups opened since the last save()

    constructor(
        readonly width: number,
        readonly height: number,
        background: string,
    ) {
        this.parts.push(`<rect width="100%" height="100%" fill="${escapeXml(background)}"/>`);
    }

    setTransform() {} // the SVG is drawn at CSS pixels
    clearRect() {}

    save() {
        this.stack.push({ state: this.state(), groups: this.groups });
        this.groups = 0;
    }

    restore() {
        const saved = this.stack.pop();
        if (!saved) return;
        this.parts.push('</g>'.repeat(this.groups));
        Object.assign(this, saved.state);
        this.groups = saved.groups;
    }

    beginPath() {
        this.path = '';
    }

    moveTo(x: number, y: number) {
        this.path += `M${n(x)} ${n(y)}`;
    }

    lineTo(x: number, y: number) {
        this.path += `L${n(x)} ${n(y)}`;
    }

    rect(x: number, y: number, w: number, h: number) {
        this.path += `M${n(x)} ${n(y)}h${n(w)}v${n(h)}h${n(-w)}Z`;
    }

    arc(x: number, y: number, r: number) {
        // Whole circles only, as the plot draws them.
        this.path += `M${n(x - r)} ${n(y)}a${n(r)} ${n(r)} 0 1 0 ${n(2 * r)} 0a${n(r)} ${n(r)} 0 1 0 ${n(-2 * r)} 0Z`;
    }

    clip() {
        const id = `clip${++this.clips}`;
        this.parts.push(`<clipPath id="${id}"><path d="${this.path}"/></clipPath><g clip-path="url(#${id})">`);
        this.groups++;
    }

    fill() {
        this.parts.push(`<path d="${this.path}" fill="${escapeXml(this.fillStyle)}"${this.alpha()}/>`);
    }

    stroke() {
        this.parts.push(`<path d="${this.path}" fill="none"${this.strokeAttributes()}/>`);
    }

    fillRect(x: number, y: number, w: number, h: number) {
        this.parts.push(
            `<rect x="${n(x)}" y="${n(y)}" width="${n(w)}" height="${n(h)}" fill="${escapeXml(this.fillStyle)}"${this.alpha()}/>`,
        );
    }

    strokeRect(x: number, y: number, w: number, h: number) {
        this.parts.push(
            `<rect x="${n(x)}" y="${n(y)}" width="${n(w)}" height="${n(h)}" fill="none"${this.strokeAttributes()}/>`,
        );
    }

    fillText(text: string, x: number, y: number) {
        const anchor =
            this.textAlign === 'center'
                ? 'middle'
                : this.textAlign === 'right' || this.textAlign === 'end'
                  ? 'end'
                  : 'start';
        const baseline =
            this.textBaseline === 'middle' ? 'central' : this.textBaseline === 'top' ? 'hanging' : 'alphabetic';
        const [, size = '10px', family = 'sans-serif'] = /^\s*(\S+)\s+(.*)$/.exec(this.font) ?? [];
        this.parts.push(
            `<text x="${n(x)}" y="${n(y)}" text-anchor="${anchor}" dominant-baseline="${baseline}" ` +
                `font-size="${escapeXml(size)}" font-family="${escapeXml(family)}" fill="${escapeXml(this.fillStyle)}"${this.alpha()}>` +
                `${escapeXml(text)}</text>`,
        );
    }

    /** The drawing as a standalone SVG document. */
    toString() {
        return (
            `<svg xmlns="${SVG_NS}" width="${this.width}" height="${this.height}" viewBox="0 0 ${this.width} ${this.height}">` +
            this.parts.join('') +
            '</g>'.repeat(this.groups) +
            '</svg>'
        );
    }

    private state(): SvgState {
        const { fillStyle, strokeStyle, lineWidth, lineCap, globalAlpha, font, textAlign, textBaseline } = this;
        return { fillStyle, strokeStyle, lineWidth, lineCap, globalAlpha, font, textAlign, textBaseline };
    }

    private alpha() {
        return this.globalAlpha < 1 ? ` opacity="${n(this.globalAlpha)}"` : '';
    }

    private strokeAttributes() {
        const cap = this.lineCap === 'butt' ? '' : ` stroke-linecap="${this.lineCap}"`;
        return ` stroke="${escapeXml(this.strokeStyle)}" stroke-width="${n(this.lineWidth)}"${cap}${this.alpha()}`;
    }
}

type SvgState = Pick<
    SvgContext,
    'fillStyle' | 'strokeStyle' | 'lineWidth' | 'lineCap' | 'globalAlpha' | 'font' | 'textAlign' | 'textBaseline'
>;

// Coordinates to two decimals, which keeps the files small.
function n(value: number) {
    return String(Math.round(value * 100) / 100);
}
