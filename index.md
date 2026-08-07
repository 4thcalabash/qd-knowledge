---
layout: default
title: 首页
---

QD Cheatsheet, not a Tutorial.

> 本博客由 AI 帮助编写，所有内容均为实际面试中遇到的问题，或经实际工作验证有效的方法。

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
