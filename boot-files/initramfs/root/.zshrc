export TERM=linux
export TERMINFO=/usr/share/terminfo
export HOME=/root
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin

PROMPT='%F{green}%n@fonteos%f %F{blue} %~%f %# '
HISTSIZE=1000

bindkey -e
bindkey '^?' backward-delete-char
bindkey '^H'  backward-delete-char
bindkey '^[[A' history-search-backward
bindkey '^[[B' history-search-forward
bindkey '^[[C' forward-char
bindkey '^[[D' backward-char

sysfetch 2>/dev/null
