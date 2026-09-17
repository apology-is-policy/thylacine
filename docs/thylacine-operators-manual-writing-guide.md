# Thylacine Operator’s Manual: Writing Guide

## Purpose

Write a comprehensive Operator’s Manual for Thylacine.

The manual must serve both as a continuous technical book and as a collection of independently useful reference sections. A reader should be able to read it sequentially to understand the system, or open a single section to perform a task, check a rule, or investigate a failure.

Use the register of a well-written language or library reference, with [cppreference](https://en.cppreference.com/cpp/language/copy_elision) as the principal stylistic reference. Combine its precise definitions, explicit conditions, and carefully bounded statements with connected explanatory prose.

The desired authority comes from technical accuracy and clear relationships between facts. Use ordinary technical vocabulary, natural sentence rhythm, and a deliberate mixture of active and passive voice.

## The reader

Assume a technically interested operator who is comfortable with commands, files, processes, and configuration, but who does not already know Thylacine’s architecture or terminology.

Some readers will want only a working invocation. Others will want to understand the mechanism behind it. Support both without requiring the first group to read the architectural explanation.

Introduce Thylacine-specific concepts when they first become necessary. Familiar terminology may still require explanation where Thylacine gives it a different meaning or behaviour.

Do not assume that experience with Linux, POSIX, or Plan 9 establishes the correct expectation. State relevant differences at the point where they affect the reader’s task.

## Structure of each section

Each independently addressable section follows this structure:

- A title naming the facility, subsystem, program, or subject.
- Opening overview paragraphs, without an “Overview” heading.
- **In Practice**
- **Technical Details**

Do not number these headings or their subsections.

Use descriptive subheadings within the two named parts where they improve navigation. Keep the three-part structure consistent while allowing the amount and arrangement of material within each part to follow the subject.

### Opening overview

Begin immediately with a concrete definition of the subject and its purpose. Explain what it enables the operator to do and when it should be used.

Establish the principal modes of use and introduce the concepts needed to distinguish them. Briefly identify neighbouring facilities where their relationship is necessary to understand the subject.

The opening should allow the reader to determine:

- What the facility is.
- What tasks it supports.
- Which form of use is appropriate.
- What essential prerequisites or limitations affect that choice.

Keep implementation details out of the opening unless they explain an immediately relevant property. An overview of a container facility, for example, should explain its execution environment and forms of use before discussing its internal dispatch path.

Use enough prose to establish the subject properly. Do not compress every overview into a slogan or a single crowded paragraph.

### In Practice

Present the most useful real-world tasks first. A reader looking for a common invocation should encounter it before a complete list of options or an architectural explanation.

Organise this part by task. Use headings such as:

- Run an installed Linux utility
- Enter an interactive container
- Configure the entrypoint
- Inspect the current namespace
- Recover from a failed mount

Each example should perform meaningful work. Prefer a small number of representative examples to many slight variations of the same command.

For each task, supply the relevant combination of:

- Prerequisites and required resources.
- The command or configuration.
- An explanation of significant arguments.
- The expected result.
- A way to confirm success, where useful.
- Scope, persistence, and side effects.
- Likely failures and the appropriate response.
- Cleanup or reversal, when the task requires it.

These are content obligations, not mandatory labels to repeat under every example. Integrate them into readable prose.

Place essential conditions before the action that depends on them. State consequential side effects before the reader executes the command.

After the common tasks, include the compact reference material needed for completeness: operands, options, configuration fields, defaults, exit statuses, limits, and errors. Use tables where the entries are genuinely parallel.

Common tasks should be usable without reading the entire reference table. Unusual cases should still be documented precisely enough to look up.

### Technical Details

Explain how the system produces the behaviour described in the practical part.

Write for an operator interested in operating-system design. This part should support understanding and diagnosis without becoming a source-code tour.

Identify the main components and follow a representative operation through them. Explain the relationships that establish observable behaviour:

- How names are resolved.
- Where state is stored and how long it exists.
- Which process or service owns a resource.
- Where decisions and permission checks occur.
- How resources cross process or service boundaries.
- What happens during failure and cleanup.
- Which ordering or concurrency rules affect the result.
- Why a restriction or compatibility limitation exists.

Use prose, tables, and diagrams where they improve the explanation.

Do not include implementation code examples, function prototypes, pseudocode, or line-by-line walkthroughs. It is appropriate to name a syscall, flag, protocol field, or internal component when the name helps identify the mechanism being explained.

Implementation details should earn their place by explaining a property the reader can use. Avoid inventories of internal structures, helper functions, and source files.

A practical limitation may be mentioned in both parts. In Practice states what the operator must account for; Technical Details explains why that condition exists.

## Reference voice

Write as an exact description of the system.

Prefer sentences that establish definitions, conditions, effects, requirements, exceptions, and consequences. Avoid promotional language, theatrical seriousness, simulated enthusiasm, and commentary about how elegant or impressive a mechanism is.

The author should rarely become a character in the text. Avoid conversational scaffolding such as:

- “Let’s take a look.”
- “As you can see.”
- “The interesting thing here is…”
- “It’s worth noting that…”
- “You might be wondering…”
- “This is where the magic happens.”

State the relevant fact directly, with whatever explanation it requires.

Do not imitate the formality of a reference by replacing common terms with elevated synonyms. Use “configuration,” “run,” “start,” “argument,” “permission,” and “root filesystem” where those are the correct terms.

An established system term should retain its spelling and meaning throughout the manual. Do not alternate between several names for the same object merely to avoid repetition.

## Active and passive voice

Choose the grammatical subject according to what the sentence explains.

Use active voice when a component’s action or responsibility matters:

> The runner reads the executable and its arguments from `config.json`.

> The kernel determines the phenotype whenever it loads a program image.

> Diorama supplies the container’s `/proc` and `/sys` trees.

Use passive voice when the object, requirement, or result is the subject of interest:

> Unknown configuration fields are ignored.

> If the working directory is omitted, `/` is used.

> The bundle pathname must be absolute.

Both forms belong in the same passage. Do not impose an active-voice or passive-voice quota.

Avoid passive constructions that obscure a necessary distinction. If the reader needs to know whether the kernel, shell, runner, or server performs an action, name that component.

Avoid unnecessary agent phrases. “The kernel selects the phenotype” is generally more natural than “The phenotype is selected by the kernel.”

Passive voice should make the sentence’s focus clearer. It should not be used to manufacture formality.

## Sentence structure and rhythm

Use sentence structure to express relationships between facts.

Conditions, causes, exceptions, and consequences often belong in the same sentence. Connect them through subordinate clauses, coordination, or a semicolon when their relationship would otherwise have to be inferred.

Do not assign a separate short sentence to every fact. A sequence of individually clear statements can still produce monotonous, disconnected prose.

For example, avoid this rhythm:

> The runner changes its root. It mounts the resources. The attachment points must exist. Assembly fails if one is missing. The runner selects the working directory. It starts the entrypoint.

A connected version is preferable:

> Once the required descriptors have been opened, the runner changes its root and mounts the resources at attachment points in the root filesystem. These attachment points must already exist; if any required point is missing, assembly fails before the entrypoint is started. After the mounts have been established, the runner selects the working directory and starts the entrypoint.

The longer sentences establish sequence and connect each condition to its consequence. Their complexity serves the explanation.

Vary sentence length according to the material. Short sentences are useful for definitions, firm restrictions, and points requiring emphasis. Longer sentences are useful when a claim depends on several related conditions.

Do not enforce a fixed maximum sentence length. Split a sentence when it contains competing main ideas, ambiguous attachments, or so many nested qualifications that the reader must reconstruct its grammar.

Use semicolons for closely related independent clauses. Use parentheses for genuinely secondary qualifications. Essential conditions usually belong in the main sentence.

Avoid repeating the same sentence pattern throughout a paragraph. A succession of “If…”, “When…”, or “The runner…” openings can become as monotonous as a succession of short statements.

## Paragraph development

Give each paragraph a coherent explanatory purpose.

A paragraph should normally establish a fact and develop its relevant conditions, mechanism, or consequences. Let successive sentences advance the explanation instead of restating the opening.

Useful paragraph structures include:

- Definition, scope, consequence.
- Condition, behaviour, exception.
- Action, reason for the ordering, failure behaviour.
- Mechanism, observable effect, limitation.

These are compositional possibilities, not templates to repeat mechanically.

Use explicit connections where the relationship matters: “because,” “although,” “unless,” “provided that,” “after,” and “otherwise.” Choose the connection that is technically true.

Do not add a concluding maxim to every paragraph. Statements such as “The declaration confers no authority” are useful when they define a boundary. Repeated as dramatic conclusions, they become mannerisms.

## Conditions, requirements, and guarantees

State the domain of each claim.

Distinguish:

- A required condition from a recommended practice.
- A default from an invariant.
- A supported operation from an unrestricted interface.
- A successful request from every property the caller might assume it establishes.
- A current implementation limit from an intentional design restriction.
- An unavailable feature from an invalid argument or denied operation.
- An observable guarantee from an implementation detail.

Use modal verbs deliberately:

| Form | Intended use |
|---|---|
| “must” / “must not” | A requirement or prohibition. |
| “is” / “does” | Specified or verified behaviour. |
| “can” | A supported capability or possibility. |
| “may” / “is permitted” | Permission or a bounded alternative, with the meaning made clear. |
| “should” | A recommendation for which alternatives remain valid. |
| “is not supported” | An implementation limitation. |
| “is not guaranteed” | A property callers must not rely on. |

Do not use formal language to strengthen an unsupported claim. “Always,” “never,” “only,” and “exactly” require evidence covering the stated scope.

Keep exceptions close to the rules they qualify. A broad claim should not remain apparently unconditional until a caveat several pages later.

Avoid ambiguous pronouns when several components or resources are involved. Repeating the technical noun is preferable to an unclear “it,” “this,” or “they.”

## Examples and commands

Examples must match the implemented interface.

Verify command names, operands, options, configuration keys, defaults, quoting rules, and path resolution against the current system. Do not import familiar Linux or POSIX syntax unless Thylacine supports it in the context shown.

Identify the shell or execution context when it affects interpretation. Commands entered in Utopia, commands entered in a Linux shell, and strings interpreted by a configured shell may obey different rules.

For each example:

- State prerequisites that are not ordinarily present.
- Distinguish installed paths from illustrative paths.
- Identify placeholders explicitly.
- Explain whether paths are resolved in the current namespace or inside a container.
- Separate commands from their output.
- Include prompt characters only when needed to distinguish contexts.
- Explain persistent changes and relevant side effects.
- Avoid adding unrelated setup that obscures the task.

Configuration examples belong in In Practice and may be shown in full when a complete example is needed. The restriction on code examples applies to implementation material in Technical Details.

Do not fabricate output to make an example appear tested. If output is illustrative or variable, label it accordingly and explain which parts matter.

Do not claim that an example was executed when it was only checked against source. Keep verification notes outside the manual’s reading flow unless a limitation must be disclosed to the reader.

## Technical accuracy and sources

Treat the repository’s current implementation and applicable technical references as the factual basis.

Read beyond the entry-point document. Follow the references needed to establish the operator-facing behaviour, including command parsing, configuration handling, startup, errors, and cleanup.

Separate current behaviour from:

- Design proposals.
- Historical descriptions.
- Superseded decisions.
- Planned features.
- Test-only interfaces.
- Incomplete implementation.

Resolve discrepancies before presenting a polished claim. Where a reference conflicts with current code, examine the relevant implementation and its tests or recorded validation. Do not silently combine incompatible descriptions.

Where practical, record the repository revision against which the manual was checked. Keep an editorial source map linking claims and examples to their evidence; source-file inventories need not appear in the reader-facing prose.

When evidence is insufficient, narrow the statement, flag the unresolved issue in editorial notes, or omit the unsupported claim. Do not fill gaps with plausible behaviour.

The reference voice must follow the strength of the evidence.

## Failure and recovery

Document failures in terms the operator can observe.

For a significant failure, explain:

- What condition causes it.
- How it is reported.
- What state or partial work remains.
- What the operator should check or do next.

Distinguish errors produced by the tool from errors returned by the program it launches. Where exit statuses overlap, explain how diagnostics identify the source.

Do not claim rollback, atomicity, cleanup, retry safety, or isolation without verifying the relevant behaviour.

Place routine failure information beside the practical task. Use Technical Details to explain the mechanisms behind more complex failure behaviour.

Avoid generic advice such as “check your configuration” when the relevant field, path, permission, or dependency can be named.

## A continuous book and an independent reference

Each section must remain understandable when read alone.

Define essential local terminology and state prerequisites within the section. Cross-references should provide additional depth without being required to decode the immediate instructions.

Use descriptive references such as “See Namespace Resolution.” Avoid “as explained earlier,” “in the next section,” and similar expressions that depend on reading order.

Arrange sections so that successive topics develop the reader’s understanding. Continuity should arise from the relationship between subjects and the progression of ideas.

Do not force transitions between unrelated reference entries. A section may end with a final condition, example, or explanation; it does not require a recap or a rhetorical bridge.

Repeat a short definition or critical limitation where independent use requires it. For extensive shared material, provide a concise local explanation and a precise cross-reference.

## Beacon presentation

Produce the manual using the repository’s supported Beacon authoring workflow. The Markdown used in this guide describes editorial structure; it does not establish the manual’s source format.

Verify the current authoring conventions and available semantic forms before encoding the document. Do not invent markup, anchors, link syntax, callout types, or extensions.

Apply presentation according to meaning:

- Headings identify document structure.
- Preformatted blocks preserve commands, configuration, and output where spacing matters.
- Inline code identifies literal commands, paths, fields, flags, and values.
- Tables organise comparable entries.
- Emphasis distinguishes terms or qualifications when that distinction helps reading.

Do not encode a visual choice where a semantic form is available. The renderer controls typography.

Use interactive object references only where the object type and target are meaningful and supported. An illustrative pathname or placeholder must not be presented as though it identifies a verified live object.

The plain-text rendering must remain complete and understandable. Colour, typography, or interactivity must not carry information that disappears from the textual content.

## Language to avoid

Use established technical terms and state the relevant behaviour directly. Source documentation may contain rhetorical habits that must be removed when its contents are adapted for the manual.

### Invented technical jargon

Do not use “-bearing” compounds as explanatory jargon, including “load-bearing,” “weight-bearing,” “invariant-bearing,” “authority-bearing,” and similar constructions.

Describe the actual dependency, responsibility, or requirement. Identify what a mechanism guarantees, which operation depends on it, or what fails if it is absent.

Avoid vague technical metaphors when the precise relationship can be named. Do not invent terminology merely to give a statement greater apparent significance.

### Explicit negation contrasts

Do not use rhetorical contrast constructions such as:

- “X is A, not B.”
- “This is not A; it is B.”
- “Not merely A, but B.”
- “This is about A, not B.”

State the definition or behaviour directly. Where two concepts must be distinguished, explain each and specify the practical difference.

This restriction does not prohibit necessary negative statements. “The field is not enforced” and “The operation does not modify the file” describe behaviour that the reader needs to know. Retain such statements where technically relevant.

### Emotional emphasis and assertions of importance

Do not use statements such as:

- “X is real.”
- “This is a real guarantee.”
- “And X matters.”
- “That distinction matters.”
- “This is the whole point.”
- “This is where the design earns its keep.”
- “The guarantee is genuine.”
- “This is not a nicety.”

Replace assertions of importance with the specific consequence that makes the information useful.

Avoid intensifiers and dramatic evaluations such as “crucial,” “profound,” “remarkable,” or “powerful” when they merely tell the reader how to regard a fact. Established technical uses, such as “critical section,” retain their normal meaning.

### Examples

| Avoid | Use |
|---|---|
| “The ordering is load-bearing.” | “The descriptors must be opened before the root is changed, because their original paths are inaccessible afterward.” |
| “This is an invariant-bearing operation.” | “The operation must preserve descriptor ownership.” |
| “Diorama is a reformatter, not an authority.” | “Diorama reformats system information obtained through Thylacine’s existing access checks.” |
| “The isolation is real.” | State exactly which resources are inaccessible and which resources remain shared. |
| “The distinction matters.” | Explain the resulting difference in behaviour. |
| “This is not merely bookkeeping.” | Describe the state being maintained and the failure that would result if it were incorrect. |

### Other recurring habits

Remove language that contributes tone without contributing meaning, including:

- Elevated synonyms for ordinary technical terms.
- Promotional adjectives such as “seamless” or “robust” without a specific, relevant claim.
- Habitual three-part rhetorical lists.
- Dramatic sentence fragments.
- Unnecessary rhetorical questions.
- Claims that a mechanism is “simple,” “obvious,” or “just” something.
- Repeated reminders that a fact is important.
- Decorative metaphors and personification.
- Summary sentences that merely repeat the preceding paragraph.

### Editing rule

During revision, inspect each sentence that labels a mechanism’s importance, authenticity, strength, or role through metaphor. Replace it with a concrete statement of behaviour, dependency, scope, or consequence; delete it if the surrounding text already supplies that information.

Preserve technical detail and explanatory depth. Removing rhetorical emphasis must not reduce the prose to disconnected short statements.

## Writing and revision procedure

Before drafting, identify the section’s scope, the operator’s principal tasks, and the technical facts needed to support them.

Draft the practical examples early. They expose missing prerequisites, uncertain syntax, and architectural claims that need verification.

Then write the opening overview around the facility the examples actually demonstrate, and develop the technical account around the mechanisms that explain their behaviour.

Revise in separate passes:

1. **Correctness.** Verify interfaces, conditions, guarantees, errors, and implementation status.
2. **Usefulness.** Check that common tasks can be completed from In Practice.
3. **Structure.** Confirm that overview, practical use, and technical explanation perform distinct jobs.
4. **Prose.** Restore connections between facts, vary sentence structure, balance active and passive voice, and remove the language prohibited above.
5. **Presentation.** Check Beacon semantics, navigation, literal text, and plain-text readability.

During the prose pass, read representative paragraphs aloud. A passage that sounds like a sequence of unrelated declarations usually needs stronger grammatical connections. A passage that requires the reader to hold too many qualifications in memory needs restructuring.

## Final editorial check

Before accepting a section, verify the following:

- The opening explains what the facility does and when it is useful.
- Common tasks appear before exhaustive reference material.
- Examples use verified interfaces and identify their context.
- Preconditions and significant consequences appear before the relevant action.
- Technical Details explains mechanisms without source-code walkthroughs.
- Ordinary technical terms are used consistently.
- Active voice identifies responsibility where necessary.
- Passive voice keeps attention on objects, requirements, and results where appropriate.
- Complex sentences express real relationships between facts.
- Short sentences provide useful emphasis without dominating the rhythm.
- Conditions, exceptions, defaults, and guarantees are correctly distinguished.
- Invented “-bearing” jargon, rhetorical negation contrasts, and emotional assertions of importance have been removed.
- The section works when read independently.
- Beacon presentation preserves the meaning in plain text.
- No sentence exists solely to sound authoritative.

The finished prose should allow a reader to recover the system’s rules without reconstructing the author’s reasoning, while providing enough connected explanation to understand why those rules matter.
