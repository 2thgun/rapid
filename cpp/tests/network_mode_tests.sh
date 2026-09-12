#!/bin/sh
set -eu

# Load the production functions without starting the worker or touching Wi-Fi.
worker_functions=$(sed '/^# A boot resumes/,$d' "$1")
eval "$worker_functions"

check() (
  scenario=$1
  requested=$2
  expected=$3
  observed=
  activations=0
  home_connection() { printf '%s\n' home-profile; }
  write_state() { observed="$1${2:+:$2}"; }
  nmcli() {
    case "$*" in
      'radio wifi on') [ "$scenario" != radio-failure ] ;;
      'radio wifi off') [ "$scenario" != off-failure ] ;;
      '-w 30 connection up home-profile ifname wlan0')
        activations=$((activations + 1))
        [ "$scenario" != home-failure ] && [ "$scenario" != all-failure ] ;;
      '-w 15 connection up rapid-demo ifname wlan0')
        activations=$((activations + 1))
        [ "$scenario" != ap-failure ] && [ "$scenario" != all-failure ] ;;
      *) echo "Unexpected nmcli invocation: $*" >&2; return 1 ;;
    esac
  }
  # Match the worker's conditional call: set -e alone cannot protect it.
  if ! apply_mode "$requested"; then write_state error; fi
  if [ "$observed" != "$expected" ]; then
    echo "$scenario/$requested: expected $expected, got $observed" >&2
    exit 1
  fi
  if [ "$scenario" = radio-failure ] && [ "$activations" -ne 0 ]; then
    echo 'Connection activation continued after radio failure' >&2
    exit 1
  fi
)

check success ap ap
check success off off
check success home home
check ap-failure ap error
check off-failure off error
check home-failure home ap:home
check all-failure home error
check radio-failure ap error
check radio-failure home error

# A failed status-file operation must not publish an incomplete replacement.
(
  mktemp() { return 1; }
  if write_state ap; then
    echo 'Status-file creation failure was ignored' >&2
    exit 1
  fi
)
(
  temporary_state=$(mktemp)
  trap 'rm -f "$temporary_state"' EXIT
  mktemp() { printf '%s\n' "$temporary_state"; }
  chmod() { return 1; }
  published=no
  mv() { published=yes; }
  if write_state ap; then
    echo 'Status-file permission failure was ignored' >&2
    exit 1
  fi
  test "$published" = no
)
echo 'Wi-Fi mode success, failure and Home-to-AP recovery checks passed.'
