# SPDX-License-Identifier: Apache-2.0

"""Lets the compiler run as `python -m hzl_compiler`."""

from .cli import main

raise SystemExit(main())
