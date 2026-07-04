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

    /* data-scale="log": value<->pct through a log curve so short times get
       most of the drag travel (a 20 ms attack was sub-pixel on the linear
       0..5000 ms scale). min may be 0: the log floor is max/5000 (1 ms for
       the 5 s ranges); pct 0 returns the true min. */
    function scalePct(scale, min, max, v) {
        if (scale === 'log') {
            var lo = Math.max(min, max / 5000)
            return v <= lo ? 0 : clamp01(Math.log(v / lo) / Math.log(max / lo))
        }
        return clamp01((v - min) / (max - min))
    }
    function scaleValue(scale, min, max, pct) {
        if (scale === 'log') {
            var lo = Math.max(min, max / 5000)
            return pct <= 0 ? min : lo * Math.pow(max / lo, pct)
        }
        return min + pct * (max - min)
    }

    /* ── ADSR geometry ─────────────────────────────────────────────
       FIXED per-segment time axis: each of A/D/R spans its own 27%-wide
       log-scaled segment and the sustain hold a fixed 19%. Moving one
       handle no longer rescales the other breakpoints' positions (the
       old proportional A/(A+D+R) axis did — a major confusion source). */
    var envRange = {
        env_attack:  { min: 0, max: 5000  },
        env_decay:   { min: 0, max: 5000  },
        env_sustain: { min: 0, max: 1     },
        env_release: { min: 0, max: 10000 }
    }
    var ENV_SEG = 0.27, ENV_HOLD = 0.19

    function envNorm(sym, v) {
        var r = envRange[sym]
        return scalePct(sym === 'env_sustain' ? '' : 'log', r.min, r.max, v)
    }
    function envSegStart(sym) {
        var adsr = event.data.adsr
        if (sym === 'env_attack') return 0
        var ax = ENV_SEG * envNorm('env_attack', adsr.env_attack)
        if (sym === 'env_decay') return ax
        return ax + ENV_SEG * envNorm('env_decay', adsr.env_decay) + ENV_HOLD
    }
    /* Breakpoint anchors in the 0..1 × 0..1 envelope box (y: 0 = peak). */
    function envAnchors() {
        var adsr = event.data.adsr
        var s  = clamp01(adsr.env_sustain)
        var ax = ENV_SEG * envNorm('env_attack',  adsr.env_attack)
        var dx = ax + ENV_SEG * envNorm('env_decay', adsr.env_decay)
        var hx = dx + ENV_HOLD
        var rx = hx + ENV_SEG * envNorm('env_release', adsr.env_release)
        return {
            env_attack:  [ax, 0],
            env_decay:   [dx, 1 - s],
            env_sustain: [(dx + hx) / 2, 1 - s],
            env_release: [rx, 1]
        }
    }

    /* Per-symbol display formatting for value readouts and tooltip.
       For long milliseconds we collapse to seconds at 1000ms+ so the
       label stays compact. */
    var formats = {
        onset_threshold: { kind: 'ratio'    },
        sample_ms:       { kind: 'ms'       },
        attack_skip_ms:  { kind: 'ms'       },
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
        filter_type:     { kind: 'choice', labels: ['LP', 'HP', 'BP'] },
        hn_brightness:   { kind: 'ratio'    },
        hn_damping:      { kind: 'ratio'    },
        hn_even_odd:     { kind: 'ratio'    },
        hn_noise:        { kind: 'ratio'    },
        hn_width:        { kind: 'ratio'    }
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
                    ? scalePct(el.attr('data-scale'), min, max, value) : 0.5
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
       Fixed log-scaled axis (see envAnchors): each phase owns its
       segment, so only the dragged breakpoint moves. */
    function updateEnvelope() {
        var adsr = event.data && event.data.adsr
        if (!adsr) return
        var line = icon.find('.megalo-env-line')
        if (!line.length) return
        var fill = icon.find('.megalo-env-fill')

        var W = 1000, H = 100
        var an = envAnchors()
        var sustainY = an.env_decay[1] * H

        // Y inverted: silence sits on the floor (y=H), peak reaches up
        // to the midline (y=0). This matches the conventional synth
        // envelope shape.
        var pts = [
            [0,                              H],
            [an.env_attack[0] * W,           0],
            [an.env_decay[0] * W,            sustainY],
            [envSegStart('env_release') * W, sustainY],
            [an.env_release[0] * W,          H]
        ]
        var lineD = 'M ' + pts.map(function (p) {
            return p[0].toFixed(2) + ',' + p[1].toFixed(2)
        }).join(' L ')

        line.attr('d', lineD)
        fill.attr('d', lineD + ' Z')

        /* Park each A/D/S/R knob on its breakpoint of the curve.
           A = attack peak, D = end of decay, S = middle of the
           sustain hold, R = end of release. Each knob lives in its own
           quarter-width column, so convert to a percentage local to that
           column (the knob CSS recenters with translate(-50%,-50%)). */
        var col = 0
        for (var sym in an) {
            var knob = icon.find('.megalo-handle[data-handle="' + sym + '"] .megalo-handle-knob')[0]
            if (knob) {
                knob.style.left = ((an[sym][0] - col * 0.25) * 4 * 100).toFixed(2) + '%'
                knob.style.top  = (an[sym][1] * 100).toFixed(2) + '%'
            }
            col++
        }
    }

    if (event.type === 'start') {

        event.data.adsr = {
            env_attack:  100,
            env_decay:   200,
            env_sustain: 0.8,
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

        // The threshold line is display-only now (set via the ONSET knob), so
        // it is not registered for dragging — only the top time handles are.
        delegateDrag('.megalo-handles-top .megalo-handle', function (el) { return el })

        // ── ADSR: nearest-breakpoint drag on the envelope area itself.
        // A/D/R are TIMES → horizontal drag along the fixed axis; S is a
        // LEVEL → vertical. The grab picks the breakpoint nearest to the
        // cursor: the old fixed quarter-columns could grab the wrong
        // parameter whenever a dot sat outside its own column (A=0, D=0
        // parks A and D at the far left, in each other's columns).
        icon.on('mousedown.megalo', '.megalo-handles-bottom', function (e) {
            if (e.which && e.which !== 1) return
            var track = $(this)
            var rect0 = track[0].getBoundingClientRect()
            if (rect0.width <= 0 || rect0.height <= 0) return

            var an = envAnchors()
            var symbol = null, bestD = 24     // grab radius, px
            for (var sym in an) {
                var ddx = an[sym][0] * rect0.width  - (e.clientX - rect0.left)
                var ddy = an[sym][1] * rect0.height - (e.clientY - rect0.top)
                var d   = Math.sqrt(ddx * ddx + ddy * ddy)
                if (d < bestD) { bestD = d; symbol = sym }
            }
            if (!symbol) return
            e.preventDefault()
            e.stopPropagation()

            var r = envRange[symbol]

            function move(ev) {
                var rect = track[0].getBoundingClientRect()
                var raw
                if (symbol === 'env_sustain') {
                    raw = clamp01(1 - (ev.clientY - rect.top) / rect.height)
                } else {
                    var pct = clamp01(((ev.clientX - rect.left) / rect.width
                                       - envSegStart(symbol)) / ENV_SEG)
                    raw = scaleValue('log', r.min, r.max, pct)
                }
                funcs.set_port_value(symbol, raw)
                event.data.adsr[symbol] = raw
                updateEnvelope()
                icon.find('[data-handle-value="' + symbol + '"]').text(formatValue(symbol, raw))
                showTooltip(ev, formatValue(symbol, raw))
            }
            function up() {
                hideTooltip()
                $(document).off('mousemove.megaloenv mouseup.megaloenv')
            }
            $(document).on('mousemove.megaloenv', move)
                     .on('mouseup.megaloenv', up)
            move(e)
        })

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
