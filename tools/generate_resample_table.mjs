import {writeFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const steps = 8192, taps = 48, scale = 2 ** 30;

function nearestEven(value) {
    const lower = Math.floor(value);
    const fraction = value - lower;
    return fraction < 0.5 ? lower :
        fraction > 0.5 ? lower + 1 :
        lower % 2 ? lower + 1 : lower;
}

function table(name, values) {
    const rows = [];
    for (let i = 0; i < values.length; i += 8)
        rows.push(`    ${values.slice(i, i + 8).join(', ')},`);
    return `static const int32_t ${name}[${values.length}] = {\n` +
        `${rows.join('\n')}\n};\n`;
}

export function resampleHeader() {
    const sine = Array.from({length: steps + 1}, (_, i) =>
        nearestEven(Math.sin(Math.PI * i / steps) * scale));
    const window = Array.from({length: taps}, (_, i) =>
        nearestEven((0.42 - 0.5 * Math.cos(2 * Math.PI * i / (taps - 1)) +
                     0.08 * Math.cos(4 * Math.PI * i / (taps - 1))) * scale));
    return '#ifndef GTASADS_RESAMPLE_COEFFS_H\n' +
        '#define GTASADS_RESAMPLE_COEFFS_H\n' +
        '#include <stdint.h>\n' +
        table('gt_sine', sine) + table('gt_window', window) + '#endif\n';
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    const index = process.argv.indexOf('--output');
    const output = index < 0 ? resolve(import.meta.dirname,
        'portable/resample_coeffs.h') : process.argv[index + 1];
    if (!output) throw new Error('Usage: node tools/generate_resample_table.mjs [--output FILE]');
    await writeFile(output, resampleHeader());
}
