# Examples

## Example 1: Refining a Vague Prompt

**Before**: "Help me write a better prompt for analyzing customer feedback."

**After**: 
```
You are an expert prompt engineer. I need a prompt that:
- Analyzes customer feedback for sentiment (positive/negative/neutral)
- Extracts key themes and pain points
- Identifies actionable recommendations
- Outputs structured JSON with: sentiment, themes (array), pain_points (array), recommendations (array)
```

## Example 2: Custom Instructions for an Agent

```yaml
name: data-analysis-agent
description: Specialized agent for financial data analysis and reporting
```

**Do's**: Verify data sources, provide confidence levels, highlight assumptions, use clear tables
**Don'ts**: Don't predict beyond 12mo without caveats, don't ignore outliers, don't present correlation as causation
**Output format**: 1) Executive Summary, 2) Key Findings, 3) Detailed Analysis, 4) Limitations, 5) Recommendations

## Example 3: Few-Shot Classification

```
Classify into: billing / technical / feature_request / general

Ticket: "I was charged twice" → billing
Ticket: "App crashes on upload" → technical
Ticket: "Would love dark mode" → feature_request

Now classify: "How do I reset my password?" →
```

## Example 4: CoT for Complex Analysis
```
Step 1: Identify core problem
Step 2: Analyze contributing factors
Step 3: Evaluate 3-5 viable solutions (pros/cons/challenges)
Step 4: Recommend and justify best solution
```

## Example 5: XML-Structured Prompt
```xml
<prompt>
  <instructions>
    <objective>Create compelling marketing copy</objective>
    <constraints>
      <max_length>150 words</max_length>
      <tone>Professional but approachable</tone>
    </constraints>
    <format>
      <headline>Benefit-focused (max 10 words)</headline>
      <body>2-3 paragraphs</body>
      <cta>Clear call-to-action</cta>
    </format>
  </instructions>
</prompt>
```

## Example 6: Prompt Optimization Checklist
- [ ] Objective is crystal clear
- [ ] No ambiguous terms
- [ ] Examples provided
- [ ] Format specified
- [ ] Edge cases addressed
- [ ] Success criteria defined
- [ ] Can measure success
- [ ] Handles variations in input
