pack:
    repomix

zip out="forged-core.zip":
    git ls-files -- 'CMakeLists.txt' common elf macho | zip -q "{{out}}" -@
