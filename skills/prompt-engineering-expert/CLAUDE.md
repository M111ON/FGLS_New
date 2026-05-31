---
name: prompt-engineering-expert
description: Advanced expert in prompt engineering, custom instructions design, and prompt optimization for AI agents
---

# Prompt Engineering Expert Skill

This skill equips the agent with deep expertise in prompt engineering, custom instructions design, and prompt optimization. It provides comprehensive guidance on crafting effective AI prompts, designing agent instructions, and iteratively improving prompt performance.

## Core Expertise Areas

### 1. Prompt Writing Best Practices
- **Clarity and Directness**: Writing clear, unambiguous prompts
- **Structure and Formatting**: Organizing prompts with proper hierarchy and visual clarity
- **Specificity**: Providing precise instructions with concrete examples
- **Context Management**: Balancing necessary context without overwhelming the model
- **Tone and Style**: Matching prompt tone to the task requirements

### 2. Advanced Prompt Engineering Techniques
- **Chain-of-Thought (CoT) Prompting**: Step-by-step reasoning for complex tasks
- **Few-Shot Prompting**: Using examples (1-shot, 2-shot, multi-shot)
- **XML Tags**: Structured formatting for clarity and parsing
- **Role-Based Prompting**: Assigning specific personas or expertise
- **Prefilling**: Starting response to guide output format
- **Prompt Chaining**: Breaking complex tasks into sequential prompts

### 3. Custom Instructions & System Prompts
- **System Prompt Design**: Effective system prompts for specialized domains
- **Behavioral Guidelines**: Constraints, Do's/Don'ts, edge cases
- **Personality and Voice**: Consistent tone and communication style
- **Scope Definition**: What the agent should and shouldn't do

### 4. Prompt Optimization & Refinement
- **Performance Analysis**: Evaluating effectiveness and identifying issues
- **Iterative Improvement**: Systematically refining based on results
- **A/B Testing**: Comparing prompt variations
- **Consistency Enhancement**: Reducing variability
- **Token Optimization**: Reducing unnecessary tokens

### 5. Anti-Patterns & Common Mistakes
- **Vagueness**: Unclear instructions
- **Contradictions**: Conflicting requirements
- **Over-Specification**: Too restrictive prompts
- **Hallucination Risks**: Prompts prone to false information
- **Context Leakage**: Unintended information exposure
- **Jailbreak Vulnerabilities**: Prompt injection risks

### 6. Evaluation & Testing
- **Success Criteria Definition**: Clear metrics for prompt success
- **Test Case Development**: Happy path, edge cases, error cases, stress tests
- **Failure Analysis**: Root cause analysis
- **Regression Testing**: Ensuring improvements don't break existing functionality

### 7. Multimodal & Advanced Prompting
- **Vision Prompting**: Image analysis prompts
- **File-Based Prompting**: Working with documents, PDFs, structured data
- **Tool Use Prompting**: Designing prompts that use tools and APIs
- **Extended Thinking**: Leveraging extended thinking for complex reasoning

## Key Anti-Patterns to Avoid

| Issue | Bad | Good |
|-------|-----|------|
| Inconsistent outputs | Ambiguous instructions | Add format spec + examples |
| Hallucinations | Prompts encouraging speculation | Ask for sources + confidence levels |
| Vague responses | "Help me with this" | Add specific context + examples |
| Wrong format | No format specified | Show exact format example |
| Too long | Unnecessary context | Remove redundancy, use progressive disclosure |
| Doesn't generalize | Hardcoded values | Use variables, handle variations |

## Quick Reference: Common Techniques

| Technique | When | How |
|-----------|------|-----|
| CoT | Complex reasoning | "Let's think step by step: 1..., 2..., 3..." |
| Few-shot | Guiding format/behavior | Provide 1-5 diverse examples |
| XML tags | Structured parsing | `<task><objective>...</objective></task>` |
| Role-based | Domain expertise | "You are a [ROLE] with expertise in [DOMAIN]" |
| Prefilling | Output format control | Start response with expected structure |
| Chaining | Multi-step tasks | Split into sequential prompts |

## Debugging Workflow
1. **Identify**: What's not working? How does it fail?
2. **Analyze**: Is objective clear? Instructions specific? Context sufficient? Format specified?
3. **Test**: Try more context, more specificity, examples, format changes
4. **Fix**: Update prompt, test with multiple inputs, verify consistency
5. **Validate**: Does it generalize? Is it efficient? Maintainable?

See `docs/` for detailed guides on best practices, techniques, and troubleshooting.
