/* ============================================================
   MEGALO modgui — visual sync + custom drag/click + ADSR curve
   --------------------------------------------------------------
   data-min / data-max are hardcoded in the template because
   MOD-UI's 'start' event ships ports as { symbol, value } only.
   The ADSR cache is persisted in event.data (MOD-UI exposes
   self.jsData on every triggerJS call) so 'change' events fired
   after 'start' can read prior values to redraw the curve.
   ============================================================ */

function (event, funcs) {

    var icon = event.icon

    function clamp01(x) { return x < 0 ? 0 : x > 1 ? 1 : x }

    /* Per-symbol display formatting for value readouts and tooltip.
       For long milliseconds we collapse to seconds at 1000ms+ so the
       label stays compact. */
    var formats = {
        onset_threshold: { kind: 'ratio'    },
        sample_ms:       { kind: 'ms'       },
        attack_skip_ms:  { kind: 'ms'       },
        grain_size_ms:   { kind: 'ms'       },
        grain_xfade_ms:  { kind: 'ms'       },
        env_attack:      { kind: 'ms-long'  },
        env_decay:       { kind: 'ms-long'  },
        env_sustain:     { kind: 'ratio'    },
        env_release:     { kind: 'ms-long'  },
        base_pitch:      { kind: 'semi'     },
        pitch1_semi:     { kind: 'semi'     },
        pitch2_semi:     { kind: 'semi'     },
        pitch1_level:    { kind: 'ratio'    },
        pitch2_level:    { kind: 'ratio'    },
        blend:           { kind: 'ratio'    },
        detune_cents:    { kind: 'cents'    },
        chorus_rate:     { kind: 'hz'       },
        detune_blend:    { kind: 'ratio'    },
        filter_cutoff:   { kind: 'hz-long'  },
        filter_q:        { kind: 'ratio'    },
        filter_type:     { kind: 'choice', labels: ['LP', 'HP', 'BP'] }
    }

    function formatValue(symbol, value) {
        var f = formats[symbol]
        if (!f) return Number(value).toFixed(2)
        switch (f.kind) {
            case 'ms':       return Math.round(value) + ' ms'
            case 'ms-long':  return value >= 1000
                ? (value / 1000).toFixed(value >= 10000 ? 1 : 2) + ' s'
                : Math.round(value) + ' ms'
            case 'ratio':    return Number(value).toFixed(2)
            case 'semi':     return (value >= 0 ? '+' : '') + Math.round(value) + ' st'
            case 'cents':    return Math.round(value) + ' c'
            case 'hz':       return Number(value).toFixed(1) + ' Hz'
            case 'hz-long':  return value >= 1000
                ? (value / 1000).toFixed(1) + ' kHz'
                : Math.round(value) + ' Hz'
            case 'choice':   return f.labels[Math.round(value)] || ''
            default:         return Number(value).toFixed(2)
        }
    }

    function setVisual(symbol, value) {
        var els = icon.find('[data-handle="' + symbol + '"]')
        if (!els.length) return
        els.each(function () {
            var el = $(this)
            var min = parseFloat(el.attr('data-min'))
            var max = parseFloat(el.attr('data-max'))
            var pct = (isFinite(min) && isFinite(max) && max !== min)
                    ? clamp01((value - min) / (max - min)) : 0.5
            el[0].style.setProperty('--value', String(pct))

            if (el.hasClass('megalo-switch3')) {
                el.find('.megalo-switch3-opt').removeClass('active')
                el.find('.megalo-switch3-opt[data-value="' + Math.round(value) + '"]').addClass('active')
            }
        })

        // Live mini-readout under handles/threshold.
        var readouts = icon.find('[data-handle-value="' + symbol + '"]')
        if (readouts.length) readouts.text(formatValue(symbol, value))
    }

    /* Recompute and write the SVG path for the ADSR envelope.
       Phases A/D/R share the timeline proportionally to their
       absolute durations; the sustain hold is a fixed 18% slice
       so it stays visible at any setting. */
    function updateEnvelope() {
        var adsr = event.data && event.data.adsr
        if (!adsr) return
        var line = icon.find('.megalo-env-line')
        if (!line.length) return
        var fill = icon.find('.megalo-env-fill')

        var W = 1000, H = 100
        var a = adsr.env_attack
        var d = adsr.env_decay
        var s = clamp01(adsr.env_sustain)
        var r = adsr.env_release

        var adr = a + d + r
        if (!isFinite(adr) || adr <= 0) adr = 1
        var holdFrac = 0.18
        var dynW = W * (1 - holdFrac)
        var ax = dynW * (a / adr)
        var dx = dynW * (d / adr)
        var rx = dynW * (r / adr)
        var holdW = W * holdFrac

        // Y inverted: silence sits on the floor (y=H), peak reaches up
        // to the midline (y=0). This matches the conventional synth
        // envelope shape and the bottom-handle drag direction (up=more).
        var sustainY = (1 - s) * H
        var pts = [
            [0,                                H],
            [ax,                               0],
            [ax + dx,                          sustainY],
            [ax + dx + holdW,                  sustainY],
            [ax + dx + holdW + rx,             H]
        ]
        var lineD = 'M ' + pts.map(function (p) {
            return p[0].toFixed(2) + ',' + p[1].toFixed(2)
        }).join(' L ')

        line.attr('d', lineD)
        fill.attr('d', lineD + ' Z')

        /* Park each A/D/S/R knob on its breakpoint of the curve.
           A = attack peak, D = end of decay, S = middle of the
           sustain hold, R = end of release. Coordinates are in the
           1000x100 viewBox; each knob lives in its own quarter-width
           column, so convert to a percentage local to that column
           (the knob CSS recenters with translate(-50%,-50%)). */
        var anchors = {
            env_attack:  pts[1],
            env_decay:   pts[2],
            env_sustain: [ax + dx + holdW / 2, sustainY],
            env_release: pts[4]
        }
        var col = 0
        for (var sym in anchors) {
            var knob = icon.find('.megalo-handle[data-handle="' + sym + '"] .megalo-handle-knob')[0]
            if (knob) {
                knob.style.left = (((anchors[sym][0] / W) - col * 0.25) * 4 * 100).toFixed(2) + '%'
                knob.style.top  = ((anchors[sym][1] / H) * 100).toFixed(2) + '%'
            }
            col++
        }
    }

    if (event.type === 'start') {

        event.data.adsr = {
            env_attack:  100,
            env_decay:   200,
            env_sustain: 1,
            env_release: 100
        }

        for (var i = 0; i < event.ports.length; i++) {
            var p = event.ports[i]
            if (event.data.adsr.hasOwnProperty(p.symbol)) {
                event.data.adsr[p.symbol] = p.value
            }
            setVisual(p.symbol, p.value)
        }
        updateEnvelope()

        var tooltip = icon.find('.megalo-tooltip')[0]
        function showTooltip(ev, text) {
            if (!tooltip) return
            tooltip.textContent = text
            // position:fixed → use clientX/Y (viewport coordinates)
            tooltip.style.left = (ev.clientX + 14) + 'px'
            tooltip.style.top  = (ev.clientY - 26) + 'px'
            tooltip.style.display = 'block'
        }
        function hideTooltip() {
            if (tooltip) tooltip.style.display = 'none'
        }

        // ── Shared drag delegate (axis: x | y-up | y-down)
        function delegateDrag(selector, getTrack) {
            icon.on('mousedown.megalo', selector, function (e) {
                if (e.which && e.which !== 1) return
                e.preventDefault()
                e.stopPropagation()

                var el     = $(this)
                var symbol = el.attr('data-handle')
                var min    = parseFloat(el.attr('data-min'))
                var max    = parseFloat(el.attr('data-max'))
                var axis   = el.attr('data-axis')
                var track  = getTrack(el)

                if (!symbol || !isFinite(min) || !isFinite(max)) return

                function move(ev) {
                    var rect = track[0].getBoundingClientRect()
                    var pct
                    if (axis === 'x') {
                        if (rect.width <= 0) return
                        pct = clamp01((ev.pageX - rect.left) / rect.width)
                    } else {
                        if (rect.height <= 0) return
                        var y = clamp01((ev.pageY - rect.top) / rect.height)
                        pct = axis === 'y-up' ? 1 - y : y
                    }
                    el[0].style.setProperty('--value', String(pct))
                    var raw = min + pct * (max - min)
                    funcs.set_port_value(symbol, raw)
                    // 'from-js' isn't echoed back as 'change', so cache +
                    // redraw + refresh the readout here.
                    if (event.data.adsr.hasOwnProperty(symbol)) {
                        event.data.adsr[symbol] = raw
                        updateEnvelope()
                    }
                    icon.find('[data-handle-value="' + symbol + '"]').text(formatValue(symbol, raw))
                    showTooltip(ev, formatValue(symbol, raw))
                }
                function up() {
                    hideTooltip()
                    $(document).off('mousemove.megalo mouseup.megalo')
                }
                $(document).on('mousemove.megalo', move)
                         .on('mouseup.megalo', up)
                move(e)
            })
        }

        icon.find('.megalo-threshold').attr('data-axis', 'y-up')
        delegateDrag('.megalo-threshold', function (el) { return el.parent() })
        delegateDrag('.megalo-handle', function (el) { return el })

        // 3-way filter switch
        icon.on('mousedown.megalo', '.megalo-switch3-opt', function (e) {
            if (e.which && e.which !== 1) return
            e.preventDefault()
            e.stopPropagation()
            var opt   = $(this)
            var sw    = opt.parent()
            var value = parseInt(opt.attr('data-value'), 10)
            funcs.set_port_value(sw.attr('data-handle'), value)
            sw.find('.megalo-switch3-opt').removeClass('active')
            opt.addClass('active')
        })

    }
    else if (event.type === 'change') {
        if (event.data.adsr && event.data.adsr.hasOwnProperty(event.symbol)) {
            event.data.adsr[event.symbol] = event.value
            updateEnvelope()
        }
        // Onset trigger pulse: 0 → 1 transition fires the flash animation
        // on the threshold line. We toggle the class off then back on so
        // the @keyframes restart on every pulse.
        if (event.symbol === 'trigger_pulse' && event.value >= 0.5) {
            var thr = icon.find('.megalo-threshold')
            thr.removeClass('triggered')
            if (thr[0]) void thr[0].offsetWidth   // force reflow
            thr.addClass('triggered')
            setTimeout(function () { thr.removeClass('triggered') }, 400)
        }
        setVisual(event.symbol, event.value)
    }
}
