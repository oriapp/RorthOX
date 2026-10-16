# Configuration bounds, in both shells supported by Make.
check_command_limit() {
    local fix="$OUT/config" value want shell rc log
    mkdir -p "$fix"
    cp Makefile "$fix/Makefile"
    for value in 1 32 1023 0x20 '32   # count' 0 1024 032 -1 abc 18446744073709551648 0x10000000000000020; do
        case "$value" in 1|32|1023|0x20|'32   # count') want=0;; *) want=1;; esac
        sed '/^MAX_COMMAND_ARGUMENTS[[:space:]]*=/d' config.mk > "$fix/config.mk"
        printf '\nMAX_COMMAND_ARGUMENTS = %s\n' "$value" >> "$fix/config.mk"
        for shell in /bin/sh /bin/bash; do
            log="$fix/$(basename "$shell")-${value//[^[:alnum:]]/_}.log"
            if make -s -C "$fix" -n SHELL="$shell" build/config/rorthox-config.h > "$log" 2>&1; then rc=0; else rc=1; fi
            if [ "$rc" != "$want" ] || { [ "$rc" = 1 ] && ! grep -qE '\*\*\* .*(MAX_COMMAND_ARGUMENTS|numeric config.mk values)' "$log"; }; then
                echo "FAIL MAX_COMMAND_ARGUMENTS=$value ($shell) rc=$rc expected=$want"
                host_failed=1
            fi
        done
    done
    # It must be a shared setting, emitted into the guest C configuration.
    if ! grep -qE '^#define RORTHOX_MAX_COMMAND_ARGUMENTS (32|0x20)$' build/config/rorthox-config.h; then
        echo 'FAIL default MAX_COMMAND_ARGUMENTS is not emitted as 32'
        host_failed=1
    fi
}
