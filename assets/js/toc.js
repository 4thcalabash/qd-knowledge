// 文章侧边栏目录：从正文标题生成，滚动时高亮当前章节
// 纯前端实现，兼容 GitHub Pages（无 Jekyll 插件依赖）。
// 仅 h2/h3 进目录，保持侧边栏简洁。
(function () {
    var content = document.querySelector('.post-content');
    var sidebar = document.getElementById('toc-sidebar');
    if (!content || !sidebar) return;

    var headings = content.querySelectorAll('h2, h3');
    if (!headings.length) {
        sidebar.remove();
        return;
    }

    // 生成目录：h2 为一级，紧随其后的 h3 为二级
    var html = '<div class="toc-title">目录</div><ul>';
    var open = false;
    headings.forEach(function (h, i) {
        if (!h.id) h.id = 'sec-' + i;
        var level = h.tagName === 'H2' ? 'toc-h2' : 'toc-h3';
        if (level === 'toc-h3' && !open) {
            html += '<ul>';
            open = true;
        } else if (level === 'toc-h2' && open) {
            html += '</ul>';
            open = false;
        }
        html += '<li class="' + level + '"><a href="#' + h.id + '">' +
            h.textContent + '</a></li>';
    });
    if (open) html += '</ul>';
    html += '</ul>';
    sidebar.innerHTML = html;

    var links = sidebar.querySelectorAll('a');

    // 当前章节 = 最后一个顶部位于视口上部（header 之下）的标题
    var headerOffset = 140;
    function updateActive() {
        var idx = -1;
        headings.forEach(function (h, i) {
            if (h.getBoundingClientRect().top <= headerOffset) idx = i;
        });
        links.forEach(function (a, i) {
            a.classList.toggle('active', i === idx);
        });
    }

    // 滚动节流 + 首次定位
    var ticking = false;
    window.addEventListener('scroll', function () {
        if (!ticking) {
            window.requestAnimationFrame(function () {
                updateActive();
                ticking = false;
            });
            ticking = true;
        }
    });
    updateActive();
})();
