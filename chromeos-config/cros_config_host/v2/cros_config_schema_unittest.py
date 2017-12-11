#!/usr/bin/env python2
# Copyright 2017 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# pylint: disable=module-missing-docstring,class-missing-docstring

from __future__ import print_function

import json
import jsonschema
import os
import unittest
import re
import tempfile

from . import cros_config_schema

BASIC_CONFIG = """
reef-9042-fw: &reef-9042-fw
  bcs-overlay: 'overlay-reef-private'
  ec-image: 'Reef_EC.9042.87.1.tbz2'
  main-image: 'Reef.9042.87.1.tbz2'
  main-rw-image: 'Reef.9042.110.0.tbz2'

models:
  - name: 'basking'
    identity:
      sku-id: 0
    audio:
      main:
        cras-config-subdir: 'basking'
        ucm-suffix: 'basking'
    brand-code: 'ASUN'
    firmware:
      <<: *reef-9042-fw
      key-id: 'OEM2'
    powerd-prefs: 'reef'
    test-alias: 'reef'
"""

this_dir = os.path.dirname(__file__)

class GetNamedTupleTests(unittest.TestCase):
  def testRecursiveDicts(self):
    val = {'a': {'b': 1, 'c': 2}}
    val_tuple = cros_config_schema.GetNamedTuple(val)
    self.assertEqual(val['a']['b'], val_tuple.a.b)
    self.assertEqual(val['a']['c'], val_tuple.a.c)

  def testListInRecursiveDicts(self):
    val = {'a': {'b': [{'c': 2}]}}
    val_tuple = cros_config_schema.GetNamedTuple(val)
    self.assertEqual(val['a']['b'][0]['c'], val_tuple.a.b[0].c)

  def testDashesReplacedWithUnderscores(self):
    val = {'a-b': 1}
    val_tuple = cros_config_schema.GetNamedTuple(val)
    self.assertEqual(val['a-b'], val_tuple.a_b)


class ParseArgsTests(unittest.TestCase):
  def testParseArgs(self):
    argv = ['-s', 'schema', '-c', 'config', '-o', 'output', '-f' 'True']
    args = cros_config_schema.ParseArgs(argv)
    self.assertEqual(args.schema, 'schema')
    self.assertEqual(args.config, 'config')
    self.assertEqual(args.output, 'output')
    self.assertTrue(args.filter)


class TransformConfigTests(unittest.TestCase):
  def testBasicTransform(self):
    result = cros_config_schema.TransformConfig(BASIC_CONFIG)
    json_dict = json.loads(result)
    self.assertEqual(len(json_dict), 1)
    json_obj = cros_config_schema.GetNamedTuple(json_dict)
    self.assertEqual(1, len(json_obj.models))
    self.assertEqual(
        'basking',
        json_obj.models[0].name)
    self.assertEqual(
        '/etc/cras/basking',
        json_obj.models[0].audio.main.cras_config_dir)

class ValidateConfigSchemaTests(unittest.TestCase):
  def setUp(self):
    with open(
        os.path.join(this_dir, 'cros_config_schema.json')) as schema_stream:
      self._schema = schema_stream.read()

  def testBasicSchemaValidation(self):
    cros_config_schema.ValidateConfigSchema(
        self._schema, cros_config_schema.TransformConfig(BASIC_CONFIG))

  def testMissingRequiredElement(self):
    config = re.sub(r" *cras-config-dir: .*", "", BASIC_CONFIG)
    try:
      cros_config_schema.ValidateConfigSchema(
          self._schema, cros_config_schema.TransformConfig(config))
    except jsonschema.ValidationError as err:
      self.assertIn('required', err.__str__())
      self.assertIn('cras-config-dir', err.__str__())


class ValidateConfigTests(unittest.TestCase):
  def testBasicValidation(self):
    cros_config_schema.ValidateConfig(
        cros_config_schema.TransformConfig(BASIC_CONFIG))

  def testModelNamesNotUnique(self):
    config = """
models:
  - name: 'astronaut'
    audio:
      main:
        cras-config-subdir: 'astronaut'
  - name: 'astronaut'
    audio:
      main:
        cras-config-subdir: 'astronaut'
"""
    try:
      cros_config_schema.ValidateConfig(
          cros_config_schema.TransformConfig(config))
    except cros_config_schema.ValidationError as err:
      self.assertIn('Model names are not unique', err.__str__())


class FilterBuildElements(unittest.TestCase):
  def testBasicFilterBuildElements(self):
    json_dict = json.loads(cros_config_schema.FilterBuildElements(
        cros_config_schema.TransformConfig(BASIC_CONFIG)))
    self.assertNotIn('firmware', json_dict['models'][0])


class MainTests(unittest.TestCase):
  def testMainWithExample(self):
    output = tempfile.mktemp()
    cros_config_schema.Main(
        os.path.join(this_dir, 'cros_config_schema.json'),
        os.path.join(this_dir, 'cros_config_schema_example.yaml'),
        output)
    with open(output, 'r') as output_stream:
      with open(
          os.path.join(this_dir, 'cros_config_schema_example.json')
      ) as expected_stream:
        self.assertEqual(expected_stream.read(), output_stream.read())

    os.remove(output)


if __name__ == '__main__':
  unittest.main()
