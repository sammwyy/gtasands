function expression(pattern, paths = false) {
    let source = '^';
    for (let i = 0; i < pattern.length; i++) {
        const char = pattern[i];
        if (char === '*') {
            if (paths && pattern[i + 1] === '*' && pattern[i + 2] === '/') {
                source += '(?:[^/]+/)*';
                i += 2;
            } else {
                source += paths ? '[^/]*' : '.*';
                if (pattern[i + 1] === '*') i++;
            }
        } else if (char === '?') source += paths ? '[^/]' : '.';
        else if (char === '[') {
            let start = i + 1;
            if (pattern[start] === '!') start++;
            if (pattern[start] === ']') start++;
            const end = pattern.indexOf(']', start);
            if (end < 0) source += '\\[';
            else {
                let group = pattern.slice(i + 1, end);
                if (group.startsWith('!')) group = '^' + group.slice(1);
                source += `[${group}]`;
                i = end;
            }
        } else source += char.replace(/[\\^$+.(){}|]/g, '\\$&');
    }
    return new RegExp(source + '$', 'is');
}

export function match(name, patterns) {
    const choices = typeof patterns === 'string' ? [patterns] : patterns;
    return choices.some(pattern => expression(pattern).test(name));
}

export function matchPath(name, patterns) {
    const choices = typeof patterns === 'string' ? [patterns] : patterns;
    return choices.some(pattern => expression(pattern, true).test(name));
}

export class Parameters {
    constructor(manifest, files) {
        this.manifest = manifest;
        this.files = files;
        this.cache = new Map();
    }

    async ideNames(patterns) {
        const names = Array.isArray(patterns) ? patterns : [patterns];
        const key = JSON.stringify(names);
        if (this.cache.has(key)) return this.cache.get(key);
        const models = new Set();
        for (const pattern of names) {
            const paths = /[*?[]/.test(pattern) ?
                [...this.files].filter(([name]) => expression(pattern, true).test(name)) :
                [...this.files].filter(([name]) => name.toLowerCase() === pattern.toLowerCase());
            for (const [, file] of paths) {
                for (const line of (await file.text()).split(/\r?\n/)) {
                    const fields = line.split('#', 1)[0].split(',').map(field => field.trim());
                    if (fields.length >= 3 && /^[0-9]+$/.test(fields[0])) {
                        models.add(fields[1].toLowerCase() + '.dff');
                        models.add(fields[2].toLowerCase() + '.txd');
                    }
                }
            }
        }
        this.cache.set(key, models);
        return models;
    }

    async params(sectionName, handler, entry) {
        const section = this.manifest.sections[sectionName];
        if (!section || !this.manifest.defaults[handler])
        throw new Error('Unknown asset section or converter.');
        const params = {...this.manifest.defaults[handler], ...section[handler]};
        const overrides = [];
        let removed = match(entry, section.remove || []) ||
            !!(section.keep?.length && !match(entry, section.keep));
        for (const rule of section.rule || []) {
            if (rule.match && !match(entry, rule.match)) continue;
            if (rule.match_ide &&
                !(await this.ideNames(rule.match_ide)).has(entry.split('/').at(-1).toLowerCase()))
                continue;
            if (rule.match_texture) {
                for (const pattern of Array.isArray(rule.match_texture) ?
                    rule.match_texture : [rule.match_texture])
                    overrides.push([pattern, rule.texture || {}]);
                continue;
            }
            Object.assign(params, rule[handler] || {});
            if (rule.remove !== undefined) removed = !!rule.remove;
        }
        const global = this.manifest.global;
        if (handler === 'texture') params.scale *= global.texture_scale;
        else if (handler === 'model')
            params.ratio = Math.min(1, params.ratio * global.model_ratio);
        else if (handler === 'collision')
            params.face_ratio = Math.min(1, params.face_ratio * global.model_ratio);
        else if (handler === 'audio')
            params.sample_rate = Math.trunc(params.sample_rate * global.audio_rate_scale);
        return {params, overrides, removed};
    }
}
