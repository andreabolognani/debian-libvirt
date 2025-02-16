#BEGIN CREATE_CONFIG_FROM_TEMPLATE
create_config_from_template() {
    local config="$1"
    local template="$2"
    local firstver="$3"

    if [ "$4" != "--" ]; then
        echo "create_config_from_template called with the wrong number of arguments" >&2
        return 1
    fi
    for _ in $(seq 1 4); do
        shift
    done

    if [ -n "$2" ] && dpkg --compare-versions -- "$2" gt "$firstver"; then
        # The package is already configured, and the version that's been
        # configured is new enough to contain the config file
        if [ -e "$config.dpkg-backup" ]; then
            # The package had been configured in the past and has
            # subsequently been removed without purging, so a backup of
            # the config file is still present on the disk. Restore it
            mv -f "$config.dpkg-backup" "$config"
            return 0
        else
            # We're doing a regular upgrade. Don't change anything
            return 0
        fi
    else
        # We're either installing from scratch, or upgrading from a version
        # that didn't have the config file yet. Make a copy of the template
        # in the appropriate location and with the expected permissions
        install -o root -g root -m 0600 "$template" "$config"
        return 0
    fi
}
#END CREATE_CONFIG_FROM_TEMPLATE

#BEGIN REMOVE_CONFIG_FROM_TEMPLATE
remove_config_from_template() {
    local config="$1"
    local template="$2"
    local firstver="$3"

    if [ "$4" != "--" ]; then
        echo "remove_config_from_template called with the wrong number of arguments" >&2
        return 1
    fi
    for _ in $(seq 1 4); do
        shift
    done

    if [ "$1" = "remove" ] && [ -e "$config" ]; then
        # When removing the package, move the configuration file to the side
        # so that the daemon no longer sees it, but we can still restore it
        # at a later time if the package is reinstalled
        mv -f "$config" "$config.dpkg-backup"
        return 0
    fi

    if [ "$1" = "purge" ]; then
        # When purging the package, remove all traces of the configuration
        rm -f "$config" "$config.dpkg-backup"
        return 0
    fi
}
#END REMOVE_CONFIG_FROM_TEMPLATE

#BEGIN CREATE_PROTECTIVE_DIVERSION
create_protective_diversion() {
    local usrfile="$1"
    local firstver="$2"

    if [ "$3" != "--" ]; then
        echo "create_protective_diversion called with the wrong number of arguments" >&2
        return 1
    fi
    for _ in $(seq 1 3); do
        shift
    done

    # If we're upgrading from a new enough version of the package, it means
    # that usr-merge has already happened and we don't need to mess with
    # diversions at all
    if [ -n "$2" ] && dpkg --compare-versions -- "$2" gt "$firstver"; then
        return 0
    fi

    dpkg-divert \
        --no-rename \
        --divert "$usrfile.usr-is-merged" \
        --add "$usrfile"
}
#END CREATE_PROTECTIVE_DIVERSION

#BEGIN DELETE_PROTECTIVE_DIVERSION
delete_protective_diversion() {
    local usrfile="$1"
    local firstver="$2"

    if [ "$3" != "--" ]; then
        echo "delete_protective_diversion called with the wrong number of arguments" >&2
        return 1
    fi
    for _ in $(seq 1 3); do
        shift
    done

    # If the diversion doesn't exist there's nothing to clean up
    if [ -z "$(dpkg-divert --list "$usrfile")" ]; then
        return 0
    fi

    dpkg-divert \
        --no-rename \
        --divert "$usrfile.usr-is-merged" \
        --remove "$usrfile"
}
#END DELETE_PROTECTIVE_DIVERSION

#BEGIN SYSTEMD_DAEMON_RELOAD
systemd_daemon_reload() {
    if [ -z "$DPKG_ROOT" ] && [ -d /run/systemd/system ]; then
        systemctl --system daemon-reload >/dev/null || true
    fi
}
#END SYSTEMD_DAEMON_RELOAD

#BEGIN SYSTEMD_UNIT_RESTART_IF_ACTIVE
systemd_unit_restart_if_active() {
    if [ -z "$DPKG_ROOT" ] && [ -d /run/systemd/system ]; then
        for unit in "$@"; do
            if systemctl is-active -q "$unit"; then
                deb-systemd-invoke restart "$unit" >/dev/null || true
            fi
        done
    fi
}
#END SYSTEMD_UNIT_RESTART_IF_ACTIVE
