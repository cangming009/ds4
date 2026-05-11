# Directional Steering

Directional steering is a runtime activation edit for DS4.  A steering file is
a flat `f32` matrix with one normalized 4096-wide direction per layer.  During
Metal inference, ds4 can apply the edit after attention outputs, FFN outputs, or
both:

```text
y = y - scale * direction[layer] * dot(direction[layer], y)
```

Positive scale removes the represented direction.  Negative scale amplifies it.
With no steering file or zero scales, ds4 follows the normal inference path.

## Runtime Options

```text
--dir-steering-file FILE   load a 43 x 4096 f32 direction file
--dir-steering-ffn F       apply steering after FFN outputs; default is 1 when a file is provided
--dir-steering-attn F      apply steering after attention outputs; default is 0
```

The FFN output is usually the best first target because it is late enough in
each layer to represent behavior, style, and topic signals.  Attention steering
is available for experiments, but it can be more fragile.

## Building a Direction

The extractor compares two prompt sets:

* `good-file`: desired/target prompts.
* `bad-file`: contrast/control prompts.

It captures DS4 activations from the same local Metal graph used for inference,
averages target minus control, normalizes one vector per layer, and writes both
metadata JSON and the runtime `.f32` file.

```sh
python3 dir-stearing/tools/build_direction.py \
  --ds4 ./ds4 \
  --model ds4flash.gguf \
  --good-file dir-stearing/examples/italy_good.txt \
  --bad-file dir-stearing/examples/italy_bad.txt \
  --out dir-stearing/out/italy.json \
  --component ffn_out \
  --ctx 512
```

This writes:

```text
dir-stearing/out/italy.json
dir-stearing/out/italy.f32
```

To make the model talk more about Italy, amplify the target direction with a
negative scale:

```sh
./ds4 --nothink --temp 0 \
  --dir-steering-file dir-stearing/out/italy.f32 \
  --dir-steering-ffn -1.0 \
  -p "Explain how database indexes work."
```

To suppress the same concept, use a positive scale:

```sh
./ds4 --nothink --temp 0 \
  --dir-steering-file dir-stearing/out/italy.f32 \
  --dir-steering-ffn 1.0 \
  -p "Give travel examples while explaining caching."
```

## Evaluating Scales

Use the sweep helper to test several strengths on a fixed prompt set:

```sh
python3 dir-stearing/tools/run_sweep.py \
  --ds4 ./ds4 \
  --model ds4flash.gguf \
  --direction dir-stearing/out/italy.f32 \
  --prompts dir-stearing/examples/eval_prompts.txt \
  --scales "-2,-1,-0.5,0,0.5,1,2" \
  --nothink
```

Start with FFN scales between `-2` and `2`.  If the model becomes repetitive or
loses the task, the scale is too strong or the prompt sets are not cleanly
separating the concept.

## Quick Sanity Test

The 10-pair Italy example is intentionally tiny.  It is a useful smoke test for
the pipeline, but it is not strong enough to force Italy into every unrelated
answer without damaging quality.  Test it with a prompt where a country choice is
natural:

```sh
python3 dir-stearing/tools/build_direction.py \
  --ds4 ./ds4 \
  --model ds4flash.gguf \
  --good-file dir-stearing/examples/italy_good.txt \
  --bad-file dir-stearing/examples/italy_bad.txt \
  --out dir-stearing/out/italy.json \
  --component ffn_out \
  --ctx 512

./ds4 -m ds4flash.gguf \
  --dir-steering-file dir-stearing/out/italy.f32 \
  --dir-steering-ffn -3 \
  --nothink \
  --temp 0 \
  -p "what country do you like the most?"
```

A healthy result should choose Italy and mention ordinary Italy-related reasons
such as food, history, art, or landscape.  If you instead ask an unrelated
technical question, low strengths may show no visible Italy effect, while high
strengths can collapse into repetition such as repeated country names.  That is
a sign the toy prompt set is too small, not that the runtime steering machinery
is broken.

## Other Uses

Concept removal:

1. Put concept-heavy prompts in `good-file`.
2. Put neutral prompts in `bad-file`.
3. Run with a positive FFN scale.

Concept amplification:

1. Put desired concept prompts in `good-file`.
2. Put neutral prompts in `bad-file`.
3. Run with a negative FFN scale.

Abbreviation/style control:

1. Put abbreviation-heavy prompts in `good-file`, for example requests that
   answer with acronyms and terse labels.
2. Put fully spelled-out style prompts in `bad-file`.
3. Use negative scale to encourage abbreviations, positive scale to remove that
   shorthand-heavy direction.

The method is not a fine-tune.  It is a low-rank runtime edit, so it works best
for coarse behavior, topic, or style directions that are consistently present in
the activation captures.
