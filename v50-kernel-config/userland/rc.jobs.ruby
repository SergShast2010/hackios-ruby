# Background jobs started by init (v0.60c ruby).
/redwood/bin/ruby_ctld &
/var/jb/usr/sbin/sshd -D -e -h /var/root/.ssh/ssh_host_ed25519_key -o UsePAM=no -o PasswordAuthentication=no -o PermitRootLogin=prohibit-password -o AuthorizedKeysFile=/var/root/.ssh/authorized_keys &
