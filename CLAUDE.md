# SN5 cloud solver

This repository is driven by a cloud routine. Each run solves exactly one task.

## Task layout

```
tasks/<TASK_ID>/PROBLEM.md   statement
tasks/<TASK_ID>/cases.json   public cases: language, entrypoint, cases[{args, kwargs, expected}]
```

## Procedure

1. Pick the task: the TASK_ID given in the prompt, or if none is given, the
   newest directory under `tasks/` (by name) that has no `solution.*` file.
2. Run `bash scripts/setup.sh` once (installs rustc if missing).
3. Read `PROBLEM.md` and `cases.json`. Write the solution to
   `tasks/<TASK_ID>/solution.rs` (Rust) or `tasks/<TASK_ID>/solution.py` (Python).
   Rust: one std-only program with `fn main()`. Python: define the entrypoint function.
4. Check it: `python3 scripts/check.py tasks/<TASK_ID>`. Fix and repeat until all
   cases pass. Also reason about the constraints (time limits, large inputs).
5. Publish the result on its own branch, never on `main`:

   ```bash
   git checkout -b result/<TASK_ID>
   python3 scripts/check.py tasks/<TASK_ID> > tasks/<TASK_ID>/check.txt 2>&1 || true
   git add tasks/<TASK_ID>/solution.* tasks/<TASK_ID>/check.txt
   git commit -m "result <TASK_ID>"
   git push -u origin result/<TASK_ID>
   ```

6. End with one line: `RESULT <TASK_ID> <passed>/<total>`.

Do not modify anything outside `tasks/<TASK_ID>/`.
