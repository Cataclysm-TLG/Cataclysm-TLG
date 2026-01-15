#!/bin/bash

for po in lang/po/*.po; do
  lang=$(basename "$po" .po)
  translated=$(msgattrib --translated "$po" | grep -c '^msgid')
  untranslated=$(msgattrib --untranslated "$po" | grep -c '^msgid')
  echo "{\"${lang}\"sv, ${translated}, ${untranslated}},"
done
