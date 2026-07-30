---
layout: default
title: 首页
---

本博客旨在帮助老登快速复习 C++ 低延迟技术栈，故而点到为止。

> 本博客所有内容均由人（面试官）与 AI（候选人）交互生成，并经过人工审阅后发布。

⭐ 带星标的内容务必重点阅读。

## 文章列表

{% for post in site.posts %}
- **{{ post.date | date: "%Y-%m-%d" }}** — [{{ post.title }}]({{ post.url | prepend: site.baseurl }})
{% endfor %}
