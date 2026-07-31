---
layout: default
title: 首页
---

本博客旨在帮助老登快速复习 C++ 低延迟技术栈，故而点到为止。

> 本博客所有内容均由人（面试官）与 AI（候选人）交互生成，并经过人工逐字审阅和修订后发布。

⭐ 带星标的内容务必重点阅读。

## 文章列表

{% for post in site.posts %}
- **{{ post.date | date: "%Y-%m-%d" }}** — [{{ post.title }}]({{ post.url | prepend: site.baseurl }})
{% endfor %}

## 待更新专题

计划后续更新的技术专题，欢迎关注：

- **Solarflare 网卡专题**：Onload 用户态协议栈的机制与低延迟收发
- **内存序专题（SPSC 队列）**：无锁单生产者单消费者队列的内存序设计
- **多核缓存一致性协议（false sharing）**：MESI 一致性协议与伪共享的成因与规避
- **常见内存分配器特性总结**：malloc / tcmalloc / jemalloc 等分配器的特性对比与选择
