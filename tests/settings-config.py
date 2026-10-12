#!/usr/bin/env python3
"""Writes a copy of a Chimera configuration with this core's settings in it.

  settings-config.py <configuration> <output> <settings as JSON>

The settings are stored the way Chimera's settings window stores them: a map
of name to value under the adapter's type name. The core is also made the one
Chimera picks for an arcade machine, because the Cores folder may hold
another.
"""
import json
import sys

cfg = json.load(open(sys.argv[1]))
cfg.setdefault("CoreSettings", {})["Chimera.Emulation.Common.Waterbox.WaterboxCore"] = {
    "Values": json.loads(sys.argv[3])
}
cfg.setdefault("DefaultCores", {})["ARCADE"] = "MAME"
json.dump(cfg, open(sys.argv[2], "w"), indent=2)
