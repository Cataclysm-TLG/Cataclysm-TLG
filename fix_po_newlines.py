import os
import polib

def fix_newline_mismatches(input_path, output_path):
    po = polib.pofile(input_path)
    fixed_count = 0

    for entry in po:
        if entry.msgstr:
            id_str = entry.msgid
            str_str = entry.msgstr

            id_leading = id_str.startswith('\n')
            id_trailing = id_str.endswith('\n')

            str_leading = str_str.startswith('\n')
            str_trailing = str_str.endswith('\n')

            changed = False

            if id_leading and not str_leading:
                entry.msgstr = '\n' + str_str
                changed = True
            elif not id_leading and str_leading:
                entry.msgstr = str_str.lstrip('\n')
                changed = True

            str_str = entry.msgstr
            if id_trailing and not str_str.endswith('\n'):
                entry.msgstr = str_str + '\n'
                changed = True
            elif not id_trailing and str_str.endswith('\n'):
                entry.msgstr = str_str.rstrip('\n')
                changed = True

            if changed:
                fixed_count += 1

    po.save(output_path)
    print(f"Fixed {fixed_count} entries in {os.path.basename(input_path)}")

def fix_all_po_files(folder_path):
    for root, dirs, files in os.walk(folder_path):
        for file in files:
            if file.endswith('.po'):
                input_file = os.path.join(root, file)
                output_file = input_file  # Overwrite original or change to file + '_fixed.po'
                print(f"Fixing {input_file} ...")
                fix_newline_mismatches(input_file, output_file)

if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print("Usage: python fix_po_all.py path/to/po/folder")
        sys.exit(1)
    fix_all_po_files(sys.argv[1])
