# Troubleshooting Guide

## Common Prompt Issues

### Issue 1: Inconsistent Outputs
**Fix**: Add format specification + examples. Define constraints explicitly. Use role-based prompting.

### Issue 2: Hallucinations / False Info
**Fix**: Ask for sources, request confidence levels, ask for caveats. Provide factual context. Ask "What don't you know?"

### Issue 3: Vague or Unhelpful Responses
**Fix**: Be more specific. Provide relevant context. Specify output format. Give examples of good responses.

### Issue 4: Too Long or Too Short
**Fix**: Specify word/sentence count. Define scope clearly. Use format templates.

### Issue 5: Wrong Output Format
**Fix**: Specify exact format (JSON, CSV, table). Provide format examples. Use XML tags.

### Issue 6: Refuses to Respond
**Fix**: Clarify legitimate purpose. Reframe the question. Provide context explaining why you need this.

### Issue 7: Prompt Too Long
**Fix**: Remove unnecessary context. Consolidate similar points. Use references instead of full text.

### Issue 8: Doesn't Generalize
**Fix**: Use variables instead of hardcoded values. Handle multiple input formats. Add error handling.

## Quick Reference

| Problem | Quick Fix |
|---------|-----------|
| Inconsistent | Add format specification + examples |
| Hallucinations | Ask for sources + confidence levels |
| Vague | Add specific details + examples |
| Too long | Specify word count + format |
| Wrong format | Show exact format example |
| Refuses | Clarify legitimate purpose |
| Too long prompt | Remove unnecessary context |
| Doesn't generalize | Use variables + handle variations |

## Debugging Workflow
1. **Identify**: What's not working? How does it fail?
2. **Analyze**: Is objective clear? Instructions specific? Context sufficient? Format specified?
3. **Test**: Try adding context, being more specific, providing examples, changing format
4. **Fix**: Update prompt, test with multiple inputs, verify consistency
5. **Validate**: Does it work now? Does it generalize? Is it efficient?
