#!/usr/bin/env python3
"""Collector regressions; synthetic parser fixtures only, never released cards."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('collector',Path(__file__).with_name('build-knowledge-corpus.py'))
collector=importlib.util.module_from_spec(spec);spec.loader.exec_module(collector)

class ImportTests(unittest.TestCase):
    def test_heading_and_body_without_blank_line_strip_links(self):
        raw='== 描述 ==\n一个[[测试目标|显示名称]]，包含完整的正文而不残留链接标记。<ref>引用文字</ref>\n\n== 参考资料 ==\n不应进入正文的资料。'
        clean=collector.prose(raw)
        self.assertIn('显示名称',clean);self.assertNotIn('[[',clean);self.assertNotIn('引用文字',clean);self.assertNotIn('不应进入',clean)
    def test_does_not_turn_missing_formula_into_sentence(self):
        raw='一个需要公式的说明 <math>x^2</math>，不能删除公式后冒充完整知识。\n\n这里是另外的完整正文，不依赖公式或模板。'
        clean=collector.prose(raw)
        self.assertNotIn('冒充',clean);self.assertIn('另外的完整正文',clean)
    def test_short_source_is_not_padded(self):
        with self.assertRaises(ValueError):collector.extract('这段文字很短。')
    def test_duplicate_sentence_does_not_count_twice(self):
        text='这句用于模拟原始文章里重复出现的完整说明，必须避免用重复文本凑齐发布要求。'
        with self.assertRaises(ValueError):collector.extract(text*20)

if __name__=='__main__':unittest.main()
