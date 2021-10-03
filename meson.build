# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2021 Jonathan Neuschäfer
project('llfalse', ['c', 'cpp'], default_options: 'warning_level=3')

llvm = dependency('llvm')
executable('llfalse', ['llfalse.c', 'util.c'], dependencies: llvm)
shared_library('false', 'libfalse.c')
executable('falseflat', ['falseflat.c'])
