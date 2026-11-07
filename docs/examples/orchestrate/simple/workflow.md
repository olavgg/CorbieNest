# Simple: the advisor owns the run, one cheap worker does QA.
#   /model   anthropic:claude-sonnet-5-5     the coder (the main model)
#   /advisor anthropic:claude-opus-5-5       the owner, and the reviewer of the code
owner: default
coder: default
qa: default on anthropic:claude-haiku-5
code-review: default
