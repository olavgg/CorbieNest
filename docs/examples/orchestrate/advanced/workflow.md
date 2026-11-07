# Advanced: every part on the model whose price and strength fit it.
# Copy to .agents/workflow.md, the five folders of skills/ to .agents/skills/, and the effort.* lines of config.txt
# into ~/.config/corbienest/config. A line `slot: skill on MODEL` names the model; without `on`, the skill's `model:` is used.
owner: default on anthropic:claude-opus-5-5
coder: default
tests: wf-tests
qa: wf-qa
code-review: wf-code-review on anthropic:claude-opus-5-5
product-review: wf-product-review
ux-review: wf-ux-review
