# Reproduce measurements

```sh
python3 bench/measure.py --goose /absolute/path/to/pinned/goose \
  --repos-dir /absolute/path/to/goose-libs --output .build/report.json
python3 bench/measure.py --goose /absolute/path/to/pinned/goose \
  --repos-dir /absolute/path/to/goose-libs --baseline-ref 19af3f4 \
  --output .build/comparison.json
```

Omit `--repos-dir` for standalone synthetic fixtures. Real fixtures are extracted
from exact Git objects in the named local workspace, never dirty files or working
branches. The pinned Honk, glob and filesystem revisions, archive hashes, selected
path/content manifests, expected output hashes, compiler/backend identities, build
commands, executable hashes, seven raw samples and GNU time peak RSS are retained.
The runner builds candidate and optional baseline itself, warms them, alternates
variant order and verifies every result against Python's byte-regex oracle on
these common-syntax fixtures. There is no cross-engine performance comparison.

Cases cover real source trees, 1,000 tiny files (hit and miss), and lines exactly
at the 64KiB cap. Wall time includes GNU time wrapper/startup, collection, reads,
matching, escaping, writes and parent pipe reads. Peak RSS is Linux GNU time KiB,
not virtual reservations or cumulative allocation volume. No cache flush or CPU
frequency control is performed; shared-host noise matters. No speed threshold is
enforced and no slower sample is removed.

`results/initial.json` records the first full-buffer reader. `results/read-strategy.json`
records paired baseline/candidate observations after profiling motivated a metadata-
sized bounded read with one growth retry. Both preserve all six cases and raw
samples. They justify a narrow implementation decision, not a universal speed claim.

`results/profile-initial` and `results/profile-adaptive` retain Callgrind output,
annotation and recipe/source/binary identity. The fixture is 1,000 files named
0000.txt..0999.txt, each containing `needle next` plus LF, searched for `absent`.
Profiles use an optimized executable with a 16MiB stack reservation solely to fit
Valgrind address-space requirements. Production/timing builds keep default stacks.
These are simulated instruction counts, not native CPU time or throughput. Source
snapshot identities distinguish the profiling checkpoints from later source/documentation changes.
