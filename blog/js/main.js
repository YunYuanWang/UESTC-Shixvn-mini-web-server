/**
 * Mini Web Server Blog — Main JavaScript (UESTC Edition)
 */

document.addEventListener('DOMContentLoaded', function() {
    'use strict';

    // ================================================================
    // Progress Bar
    // ================================================================
    (function() {
        var bar = document.createElement('div');
        bar.className = 'progress-bar';
        document.body.prepend(bar);

        window.addEventListener('scroll', function() {
            var scrollTop = window.scrollY || document.documentElement.scrollTop;
            var docHeight = document.documentElement.scrollHeight - window.innerHeight;
            var progress = docHeight > 0 ? (scrollTop / docHeight) * 100 : 0;
            bar.style.width = Math.min(progress, 100) + '%';
        });
    })();

    // ================================================================
    // Back to Top Button
    // ================================================================
    (function() {
        var btn = document.createElement('button');
        btn.id = 'back-to-top';
        btn.innerHTML = '&#8593;';
        btn.title = '回到顶部';
        btn.style.cssText = [
            'display: none; position: fixed; bottom: 32px; right: 32px;',
            'width: 44px; height: 44px; border: none; border-radius: 50%;',
            'background: var(--uestc-blue, #003D7A); color: #fff;',
            'font-size: 22px; cursor: pointer;',
            'box-shadow: 0 4px 16px rgba(0,0,0,.15);',
            'transition: opacity .3s ease, transform .3s ease, background .3s ease;',
            'z-index: 999; opacity: 0;'
        ].join(' ');
        document.body.appendChild(btn);

        var showTimer;
        window.addEventListener('scroll', function() {
            clearTimeout(showTimer);
            if (window.scrollY > 500) {
                btn.style.display = 'block';
                showTimer = setTimeout(function() { btn.style.opacity = '1'; }, 50);
            } else {
                btn.style.opacity = '0';
                showTimer = setTimeout(function() {
                    if (window.scrollY <= 500) btn.style.display = 'none';
                }, 300);
            }
        });

        btn.addEventListener('mouseenter', function() {
            btn.style.transform = 'translateY(-3px)';
            btn.style.background = 'var(--uestc-gold, #C4962D)';
        });
        btn.addEventListener('mouseleave', function() {
            btn.style.transform = 'translateY(0)';
            btn.style.background = 'var(--uestc-blue, #003D7A)';
        });
        btn.addEventListener('click', function() {
            window.scrollTo({ top: 0, behavior: 'smooth' });
        });
    })();

    // ================================================================
    // Nav Active State
    // ================================================================
    (function() {
        var path = window.location.pathname;
        var links = document.querySelectorAll('nav a');
        links.forEach(function(link) {
            var href = link.getAttribute('href');
            if (href && (path.endsWith(href) || (path === '/blog/' && href === 'index.html'))) {
                link.classList.add('active');
            }
        });
    })();

    // ================================================================
    // Feature Cards Entrance Animation
    // ================================================================
    (function() {
        var cards = document.querySelectorAll('.feature-card');
        if (cards.length === 0 || !('IntersectionObserver' in window)) return;

        var observer = new IntersectionObserver(function(entries) {
            entries.forEach(function(entry, idx) {
                if (entry.isIntersecting) {
                    var card = entry.target;
                    card.style.opacity = '0';
                    card.style.transform = 'translateY(20px)';
                    card.style.transition = 'opacity 0.5s ease, transform 0.5s ease';
                    setTimeout(function() {
                        card.style.opacity = '1';
                        card.style.transform = 'translateY(0)';
                    }, idx * 80);
                    observer.unobserve(card);
                }
            });
        }, { threshold: 0.15 });

        cards.forEach(function(card) {
            card.style.opacity = '0';
            observer.observe(card);
        });
    })();

    // ================================================================
    // Mouse Tracking Glow Effect
    // ================================================================
    (function() {
        var glow = document.createElement('div');
        glow.id = 'mouse-glow';
        glow.style.cssText = [
            'position: fixed; top: 0; left: 0;',
            'width: 100%; height: 100%;',
            'pointer-events: none; z-index: 9997;',
            'background: radial-gradient(',
                '800px at 50% 50%,',
                'rgba(0,42,92,.12) 0%,',
                'rgba(0,42,92,.06) 35%,',
                'rgba(0,42,92,.02) 60%,',
                'transparent 75%',
            ');',
        ].join(' ');
        document.body.appendChild(glow);

        var cursor = document.createElement('div');
        cursor.id = 'mouse-cursor';
        cursor.style.cssText = [
            'position: fixed;',
            'width: 24px; height: 24px;',
            'border: 2px solid rgba(196,150,45,.6);',
            'border-radius: 50%;',
            'pointer-events: none; z-index: 9998;',
            'transform: translate(-50%, -50%);',
            'transition: width .2s ease, height .2s ease,',
            '           border-color .2s ease, background .2s ease;',
        ].join(' ');
        document.body.appendChild(cursor);

        var cursorDot = document.createElement('div');
        cursorDot.id = 'mouse-dot';
        cursorDot.style.cssText = [
            'position: fixed;',
            'width: 6px; height: 6px;',
            'background: rgba(196,150,45,.9);',
            'border-radius: 50%;',
            'pointer-events: none; z-index: 9999;',
            'transform: translate(-50%, -50%);',
        ].join(' ');
        document.body.appendChild(cursorDot);

        var mx = 0, my = 0;

        document.addEventListener('mousemove', function(e) {
            mx = e.clientX;
            my = e.clientY;

            // Dot & ring follow instantly (no lag = no edge bounce)
            cursorDot.style.left = mx + 'px';
            cursorDot.style.top  = my + 'px';
            cursor.style.left    = mx + 'px';
            cursor.style.top     = my + 'px';

            // Glow follows mouse
            glow.style.background =
                'radial-gradient(' +
                '800px at ' + mx + 'px ' + my + 'px,' +
                'rgba(0,42,92,.12) 0%,' +
                'rgba(0,42,92,.06) 35%,' +
                'rgba(0,42,92,.02) 60%,' +
                'transparent 75%' +
                ')';
        });

        // Scale ring on hover over interactive elements
        var hoverTargets = 'a, button, .feature-card, table tbody tr, pre, .logo';
        document.addEventListener('mouseover', function(e) {
            if (e.target.closest(hoverTargets)) {
                cursor.style.width  = '40px';
                cursor.style.height = '40px';
                cursor.style.borderColor = 'rgba(196,150,45,.9)';
                cursor.style.background  = 'rgba(196,150,45,.1)';
            }
        });
        document.addEventListener('mouseout', function(e) {
            if (e.target.closest(hoverTargets)) {
                cursor.style.width  = '24px';
                cursor.style.height = '24px';
                cursor.style.borderColor = 'rgba(196,150,45,.6)';
                cursor.style.background  = 'transparent';
            }
        });

        // ================================================================
        // Click Sparkle Effect
        // ================================================================
        document.addEventListener('click', function(e) {
            var colors = [
                'rgba(196,150,45,.9)',   // gold
                'rgba(196,150,45,.5)',
                'rgba(0,42,92,.8)',       // uestc blue
                'rgba(232,213,163,.8)',   // light gold
                'rgba(26,92,158,.7)',     // medium blue
            ];
            var count = 8;
            for (var i = 0; i < count; i++) {
                var particle = document.createElement('div');
                var angle  = (Math.PI * 2 * i) / count + Math.random() * 0.3;
                var distance = 40 + Math.random() * 55;
                var size = 4 + Math.random() * 5;
                var color = colors[Math.floor(Math.random() * colors.length)];

                // Set initial state
                particle.style.position   = 'fixed';
                particle.style.left       = e.clientX + 'px';
                particle.style.top        = e.clientY + 'px';
                particle.style.width      = size + 'px';
                particle.style.height     = size + 'px';
                particle.style.background = color;
                particle.style.borderRadius = '50%';
                particle.style.pointerEvents = 'none';
                particle.style.zIndex     = '9999';
                particle.style.transform  = 'translate(-50%, -50%) scale(1)';
                particle.style.opacity    = '1';
                // NO transition yet — add it after a frame

                document.body.appendChild(particle);

                // Force reflow so browser registers the initial state
                particle.offsetHeight;

                // Now add transition and move to final state
                particle.style.transition = 'all .6s cubic-bezier(.25,.46,.45,.94)';
                particle.style.left   = (e.clientX + Math.cos(angle) * distance) + 'px';
                particle.style.top    = (e.clientY + Math.sin(angle) * distance) + 'px';
                particle.style.opacity = '0';
                particle.style.transform = 'translate(-50%, -50%) scale(0)';

                // Clean up after animation
                setTimeout(function() {
                    if (particle.parentNode) particle.parentNode.removeChild(particle);
                }, 650);
            }
        });

        // Hide default cursor on desktop
        if (window.matchMedia('(pointer: fine)').matches) {
            document.body.style.cursor = 'none';
            document.querySelectorAll(hoverTargets).forEach(function(el) {
                el.style.cursor = 'none';
            });
            // Also hide cursor on elements added dynamically
            var style = document.createElement('style');
            style.textContent = 'a, button, .feature-card, table tbody tr, pre, .logo { cursor: none !important; }';
            document.head.appendChild(style);
        }
    })();
    (function() {
        var footer = document.querySelector('footer p');
        if (footer) {
            footer.innerHTML = footer.textContent.replace('2026', new Date().getFullYear().toString());
        }
    })();

    // ================================================================
    // Log
    // ================================================================
    console.log(
        '%c成电实验博客 %cMiniWeb/1.2 %c' + new Date().toISOString(),
        'color: #003D7A; font-weight: bold; font-size: 14px;',
        'color: #C4962D; font-weight: bold;',
        'color: #999;'
    );
});
