# Prompt Engineering Expert - Best Practices Guide

## Core Principles

### 1. Clarity and Directness
- Be explicit: State exactly what you want done
- Avoid ambiguity: Use precise language
- Use concrete examples: Show, don't just tell
- Structure logically: Organize information hierarchically

### 2. Conciseness
- Respect context windows: Keep prompts focused
- Remove redundancy: Eliminate unnecessary repetition
- Progressive disclosure: Provide details only when needed
- Token efficiency: Optimize for both quality and cost

### 3. Appropriate Degrees of Freedom
- Define constraints: Set clear boundaries
- Specify format: Be explicit about desired output format
- Set scope: Clearly define what's in and out of scope
- Balance flexibility: Allow room for reasoning while maintaining control

## Advanced Techniques

### Chain-of-Thought (CoT)
Encourage step-by-step reasoning for complex tasks.

### Few-Shot Prompting
Use examples to guide behavior: 1-shot for simple, 2-shot for moderate, multi-shot for complex patterns.

### XML Tags for Structure
```xml
<task>
  <objective>What you want done</objective>
  <constraints>Limitations and rules</constraints>
  <format>Expected output format</format>
</task>
```

### Role-Based Prompting
Assign expertise: "You are an expert [ROLE] with deep knowledge of..."

### Prefilling
Start response to guide format: "Here's my analysis:\n\nKey findings:"

### Prompt Chaining
Break complex tasks into sequential prompts: 1) Analyze input → 2) Process analysis → 3) Generate output

## Skill Structure Best Practices

### Naming Conventions
- Use gerund form (verb + -ing): "analyzing-financial-statements"
- Use lowercase with hyphens: "prompt-engineering-expert"
- Be descriptive, avoid generic names

### Progressive Disclosure Patterns
- Pattern 1: High-level guide with references to detailed sections
- Pattern 2: Domain-specific organization grouped by use case
- Pattern 3: Conditional details shown based on context

## Evaluation & Testing

### Success Criteria
- Measurable: Define what "success" looks like
- Specific: Avoid vague metrics
- Testable: Can be verified objectively
- Realistic: Achievable with the prompt

### Test Case Types
- Happy path: Normal, expected usage
- Edge cases: Boundary conditions
- Error cases: Invalid inputs
- Stress tests: Complex scenarios

## Anti-Patterns

- Vagueness: "Help me with this task"
- Contradictions: Conflicting requirements
- Over-specification: Too many constraints
- Hallucination risks: Prompts encouraging false info
- Context leakage: Unintended info exposure
- Jailbreak vulnerabilities: Susceptible to manipulation

## Token Budget Considerations
- Skill metadata: ~100-200 tokens
- Main instructions: ~500-1000 tokens
- Reference files: ~1000-5000 tokens each
- Examples: ~500-1000 tokens each

## Checklist
- [ ] Clear, specific name (gerund form)
- [ ] Concise description (1-2 sentences)
- [ ] Well-organized structure
- [ ] Progressive disclosure
- [ ] Consistent terminology
- [ ] Clear use cases defined
- [ ] Examples provided
- [ ] Edge cases documented
- [ ] Limitations stated
- [ ] Test cases created
- [ ] Success criteria defined
