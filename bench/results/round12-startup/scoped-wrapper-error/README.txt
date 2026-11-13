Both complete round7 invocations and all six intensified debug stress commands
returned success (set -e advanced past each). However, this verification wrapper
was edited while Bash was executing it to add log truncation. Bash later read a
shifted closing 'done' and reported a syntax error after all test commands:
  bench/round12/verify.sh: line 30: syntax error near unexpected token `done'
The EXIT trap restored the release build. These logs are retained as historical
evidence, not the final clean wrapper run. The final script passed bash -n and
was copied to work/round12/verify-final.sh, then run unchanged to completion.
The top-level verification logs belong to that final frozen-copy invocation.
