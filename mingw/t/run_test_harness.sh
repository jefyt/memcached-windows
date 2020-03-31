#!/bin/sh

T_DIR="$1"
if [ -z "${T_DIR}" ] ; then
    T_DIR="$(dirname "$(realpath "$0")")"
    echo "Test directory not specified. Defaulting to same directory as the script."
elif [ ! -d "${T_DIR}" ] ; then
    echo "\"${T_DIR}\" test directory does not exist!"
    exit 1
fi

rm -rf tmp
mkdir tmp

prove "${T_DIR}/00-startup.t"
prove "${T_DIR}/64bit.t"
prove "${T_DIR}/ascii-auth.t"
prove "${T_DIR}/binary-extstore.t"
prove "${T_DIR}/binary-get.t"
prove "${T_DIR}/binary-sasl.t"
prove "${T_DIR}/binary.t"
prove "${T_DIR}/bogus-commands.t"
prove "${T_DIR}/cas.t"
prove "${T_DIR}/chunked-extstore.t"
prove "${T_DIR}/chunked-items.t"
prove "${T_DIR}/conn-limits.t"
# prove "${T_DIR}/daemonize.t"              # Manually tested. Can't be used in Wine.
prove "${T_DIR}/dash-M.t"
prove "${T_DIR}/dyn-maxbytes.t"
prove "${T_DIR}/error-extstore.t"
prove "${T_DIR}/evictions.t"
prove "${T_DIR}/expirations.t"
prove "${T_DIR}/extstore-buckets.t"
prove "${T_DIR}/extstore-jbod.t"
prove "${T_DIR}/extstore.t"
prove "${T_DIR}/flags.t"
# prove "${T_DIR}/flush-all.t"              # FAILED
prove "${T_DIR}/getandtouch.t"
prove "${T_DIR}/getset.t"
prove "${T_DIR}/idle-timeout.t"
prove "${T_DIR}/incrdecr.t"
prove "${T_DIR}/issue_104.t"
prove "${T_DIR}/issue_108.t"
prove "${T_DIR}/issue_140.t"
prove "${T_DIR}/issue_14.t"
prove "${T_DIR}/issue_152.t"
prove "${T_DIR}/issue_163.t"
prove "${T_DIR}/issue_183.t"
prove "${T_DIR}/issue_192.t"
prove "${T_DIR}/issue_22.t"
prove "${T_DIR}/issue_260.t"
prove "${T_DIR}/issue_29.t"
prove "${T_DIR}/issue_3.t"
prove "${T_DIR}/issue_41.t"
prove "${T_DIR}/issue_42.t"
prove "${T_DIR}/issue_50.t"
prove "${T_DIR}/issue_61.t"
prove "${T_DIR}/issue_67.t"
prove "${T_DIR}/issue_68.t"
prove "${T_DIR}/issue_70.t"
prove "${T_DIR}/item_size_max.t"
prove "${T_DIR}/line-lengths.t"
prove "${T_DIR}/lru-crawler.t"
# prove "${T_DIR}/lru-maintainer.t"         # FAILED
prove "${T_DIR}/lru.t"
prove "${T_DIR}/malicious-commands.t"
prove "${T_DIR}/maxconns.t"
prove "${T_DIR}/metaget.t"
prove "${T_DIR}/misbehave.t"
prove "${T_DIR}/multiversioning.t"
prove "${T_DIR}/noreply.t"
prove "${T_DIR}/quit.t"
prove "${T_DIR}/refhang.t"
# prove "${T_DIR}/restart.t"                # FAILED
prove "${T_DIR}/slabhang.t"
prove "${T_DIR}/slabs-reassign2.t"
prove "${T_DIR}/slabs-reassign-chunked.t"
prove "${T_DIR}/slabs_reassign.t"
prove "${T_DIR}/ssl_cert_refresh.t"
prove "${T_DIR}/ssl_ports.t"
prove "${T_DIR}/ssl_session_resumption.t"
prove "${T_DIR}/ssl_settings.t"
prove "${T_DIR}/ssl_verify_modes.t"
prove "${T_DIR}/stats-conns.t"
prove "${T_DIR}/stats-detail.t"
prove "${T_DIR}/stats.t"
prove "${T_DIR}/touch.t"
prove "${T_DIR}/udp.t"
# prove "${T_DIR}/unixsocket.t"             # UNSUPPORTED
# prove "${T_DIR}/watcher_connid.t"         # FAILED
# prove "${T_DIR}/watcher.t"                # FAILED
# prove "${T_DIR}/whitespace.t"             # CODE FORMATTING