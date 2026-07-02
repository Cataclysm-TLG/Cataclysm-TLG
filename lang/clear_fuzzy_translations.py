#!/usr/bin/env python3

from pathlib import Path
import polib

for po_path in Path("po").glob("*.po"):
    po = polib.pofile(str(po_path))
    modified = False

    for entry in po:
        if "fuzzy" in entry.flags:
            entry.flags.remove("fuzzy")

            if entry.msgid_plural:
                for key in entry.msgstr_plural:
                    entry.msgstr_plural[key] = ""
            else:
                entry.msgstr = ""

            modified = True

    if modified:
        print(f"Updated {po_path}")
        po.save()