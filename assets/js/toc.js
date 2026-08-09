// 文章侧边栏目录（手风琴式）：
// - 顶层标题（h2）始终全部显示，作为文章骨架
// - 滚动时展开当前章节路径：从当前标题向上每一级 li 的子树均展开
//   （当前 h2 的 h3 子项、当前 h3 的 h4 子项…即当前位置的上下级都可见）
// - 其余子树收起，侧边栏保持紧凑
// 纯前端实现，兼容 GitHub Pages。
(function () {
    var content = document.querySelector('.post-content');
    var sidebar = document.getElementById('toc-sidebar');
    if (!content || !sidebar) return;

    var headings = content.querySelectorAll('h2, h3, h4');
    if (!headings.length) {
        sidebar.remove();
        return;
    }

    // 建树：h3 归入其前最近的 h2，h4 归入其前最近的 h3
    var tree = [];  // [{el, items:[{el, items:[el]}]}]
    var cur2 = null, cur3 = null;
    headings.forEach(function (h) {
        if (!h.id) h.id = 'sec-' + h.textContent.trim().replace(/\s+/g, '-').slice(0, 24);
        if (h.tagName === 'H2') {
            cur2 = { el: h, items: [] };
            tree.push(cur2);
            cur3 = null;
        } else if (h.tagName === 'H3') {
            if (!cur2) { cur2 = { el: h, items: [] }; tree.push(cur2); }
            cur3 = { el: h, items: [] };
            cur2.items.push(cur3);
        } else if (h.tagName === 'H4') {
            if (!cur3) {
                if (!cur2) { cur2 = { el: h, items: [] }; tree.push(cur2); }
                cur3 = { el: h, items: [] };
                cur2.items.push(cur3);
            }
            cur3.items.push(h);
        }
    });

    // 渲染；子树默认收起（collapsed）
    function render() {
        var html = '<div class="toc-title">目录</div><ul>';
        tree.forEach(function (h2) {
            html += '<li class="toc-h2"><a href="#' + h2.el.id + '">' +
                h2.el.textContent + '</a>';
            if (h2.items.length) {
                html += '<ul class="toc-sub collapsed">';
                h2.items.forEach(function (h3) {
                    html += '<li class="toc-h3"><a href="#' + h3.el.id + '">' +
                        h3.el.textContent + '</a>';
                    if (h3.items.length) {
                        html += '<ul class="toc-sub collapsed">';
                        h3.items.forEach(function (h4) {
                            html += '<li class="toc-h4"><a href="#' + h4.el.id + '">' +
                                h4.el.textContent + '</a></li>';
                        });
                        html += '</ul>';
                    }
                    html += '</li>';
                });
                html += '</ul>';
            }
            html += '</li>';
        });
        html += '</ul>';
        sidebar.innerHTML = html;
    }
    render();

    // 标题 id -> 侧边栏链接 映射
    var linkByHeading = {};
    sidebar.querySelectorAll('a').forEach(function (a) {
        linkByHeading[a.getAttribute('href').slice(1)] = a;
    });

    // 当前标题 = 最后一个顶部位于视口上部（header 之下）的标题
    var headerOffset = 140;
    function currentHeading() {
        var idx = -1;
        headings.forEach(function (h, i) {
            if (h.getBoundingClientRect().top <= headerOffset) idx = i;
        });
        return idx >= 0 ? headings[idx] : null;
    }

    // 展开当前项路径：从当前项 li 向上，每层 li 的子树均展开
    function expandFrom(link) {
        var li = link.parentElement;
        while (li && li.tagName === 'LI') {
            var sub = Array.prototype.slice.call(li.children)
                .find(function (c) { return c.classList.contains('toc-sub'); });
            if (sub) sub.classList.remove('collapsed');
            var ul = li.parentElement;
            li = ul ? ul.parentElement : null;
        }
    }

    function updateActive() {
        var cur = currentHeading();
        if (!cur) return;

        // 收起所有子树
        sidebar.querySelectorAll('ul.toc-sub').forEach(function (ul) {
            ul.classList.add('collapsed');
        });

        // 高亮当前 + 展开其路径
        sidebar.querySelectorAll('a.active').forEach(function (a) {
            a.classList.remove('active');
        });
        var ca = linkByHeading[cur.id];
        if (ca) {
            ca.classList.add('active');
            expandFrom(ca);
        }
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
