/**
 * Mini Web Server Blog — Main JavaScript
 */

document.addEventListener('DOMContentLoaded', function() {
    'use strict';

    // ---- Add current year to footer ----
    var footerYear = document.querySelector('footer p');
    if (footerYear) {
        var year = new Date().getFullYear();
        footerYear.innerHTML = footerYear.innerHTML.replace('2026', year.toString());
    }

    // ---- Add "back to top" button ----
    var backBtn = document.createElement('button');
    backBtn.id = 'back-to-top';
    backBtn.innerHTML = '↑';
    backBtn.title = '回到顶部';
    backBtn.style.cssText = [
        'display: none',
        'position: fixed',
        'bottom: 30px',
        'right: 30px',
        'width: 44px',
        'height: 44px',
        'border: none',
        'border-radius: 50%',
        'background: #0f3460',
        'color: #fff',
        'font-size: 20px',
        'cursor: pointer',
        'box-shadow: 0 2px 8px rgba(0,0,0,0.2)',
        'transition: opacity 0.3s ease',
        'z-index: 999'
    ].join(';');
    document.body.appendChild(backBtn);

    window.addEventListener('scroll', function() {
        if (window.scrollY > 400) {
            backBtn.style.display = 'block';
            backBtn.style.opacity = '1';
        } else {
            backBtn.style.opacity = '0';
            setTimeout(function() {
                if (window.scrollY <= 400) {
                    backBtn.style.display = 'none';
                }
            }, 300);
        }
    });

    backBtn.addEventListener('click', function() {
        window.scrollTo({ top: 0, behavior: 'smooth' });
    });

    // ---- Highlight current nav item based on URL ----
    var currentPath = window.location.pathname;
    var navLinks = document.querySelectorAll('nav a');
    navLinks.forEach(function(link) {
        var href = link.getAttribute('href');
        if (href && currentPath.endsWith(href)) {
            link.classList.add('active');
        }
    });

    // ---- Log page load (for debugging purposes) ----
    console.log('[Blog] Page loaded: ' + document.title + ' | ' + new Date().toISOString());
    console.log('[Blog] Server: MiniWeb/1.2 | Static content served successfully');
});
