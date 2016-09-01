Example openssh policy to protect against roaming mode bug.

*NOTE* this is an old version of openssh vulnerable to CVE-2016-077{7,8}, do not
use.

Modified elfbac version exists on the "policy" branch.

Link with the tools from elfbac-arm
(https://github.com/sergeybratus/elfbac-arm/blob/master/tools/elfbac-ld/elfbac-ld.py)
with the provided policies (ssh_policy.json, sshd_policy.json) to build.
