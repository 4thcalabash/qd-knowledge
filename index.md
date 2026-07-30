---
layout: default
title: 首页
---

本博客旨在帮助新人入门量化领域的 C++ 低延迟技术栈，同时复习高频面试题目。

> 本博客所有内容均由人（面试官）与 AI（候选人）交互生成，并经过人工审阅后发布。

## 文章列表

{% for post in site.posts %}
- **{{ post.date | date: "%Y-%m-%d" }}** — [{{ post.title }}]({{ post.url | prepend: site.baseurl }})
{% endfor %}
