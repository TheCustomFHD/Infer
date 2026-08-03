/* Cross-mode prompt equivalence: chat, run and serve must render the
 * same messages to the same bytes. */
#include "../src/agent.h"
#include "../src/opts.h"
#include "../src/prof.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TMPL =
"{%- for m in messages %}<|im_start|>{{ m.role }}\n{{ m.content }}<|im_end|>\n"
"{%- endfor %}{% if add_generation_prompt %}<|im_start|>assistant\n"
"{% if not enable_thinking %}<think>\n\n</think>\n\n{% endif %}{% endif %}"
"{%- if tools %}[TOOLS:{{ tools | length }}]{% endif %}";

static int fails = 0;
static void eq(const char *what, const char *a, const char *b) {
    if (strcmp(a, b) != 0) {
        printf("FAIL %s\n  A=[%s]\n  B=[%s]\n", what, a, b);
        fails++;
    } else printf("ok   %s\n", what);
}

int main(void) {
    char err[256];
    strbuf o1, o2;
    agent_config ac;
    jj_val *m;

    memset(&ac, 0, sizeof(ac));
    ac.tmpl = TMPL;

    /* 1. thinking toggle actually changes the prompt */
    m = agent_messages();
    agent_add_msg(m, "user", "hi");
    ac.thinking = 0; agent_render(&ac, m, &o1, err, sizeof(err));
    ac.thinking = 1; agent_render(&ac, m, &o2, err, sizeof(err));
    if (strstr(o1.data, "<think>") && !strstr(o2.data, "<think>"))
        printf("ok   --think changes the rendered prompt\n");
    else { printf("FAIL --think had no effect\n"); fails++; }
    sb_free(&o1); sb_free(&o2);

    /* 2. system insertion is idempotent and ordered first */
    agent_set_system(&m, "SYS");
    agent_set_system(&m, "SYS2");
    if (m->n == 2 && strcmp(jj_dict_get(m->items[0],"content")->str,"SYS2")==0)
        printf("ok   --system replaces, does not duplicate\n");
    else { printf("FAIL system handling (n=%d)\n", m->n); fails++; }

    /* 3. raw ignores the template entirely */
    ac.raw = 1;
    agent_render(&ac, m, &o1, err, sizeof(err));
    if (!strstr(o1.data, "<|im_start|>")) printf("ok   --raw bypasses the template\n");
    else { printf("FAIL raw still templated\n"); fails++; }
    sb_free(&o1);
    ac.raw = 0;

    /* 4. raw works with no template at all (the serve/llama-bench case) */
    ac.tmpl = NULL; ac.raw = 1;
    if (agent_render(&ac, m, &o1, err, sizeof(err)) == 0)
        printf("ok   --raw works without any template\n");
    else { printf("FAIL raw needs a template\n"); fails++; }
    sb_free(&o1);
    ac.tmpl = TMPL; ac.raw = 0;

    /* 5. the same messages render identically however they were built,
     *    which is what makes chat/run/serve agree */
    {
        jj_val *a = agent_messages();
        jj_val *b;
        js_val *j;
        agent_add_msg(a, "system", "S");
        agent_add_msg(a, "user", "u1");
        agent_render(&ac, a, &o1, err, sizeof(err));

        j = js_parse("[{\"role\":\"system\",\"content\":\"S\"},"
                     "{\"role\":\"user\",\"content\":\"u1\"}]");
        b = agent_js_to_jj(j);
        agent_render(&ac, b, &o2, err, sizeof(err));
        eq("chat-built and serve-built prompts are identical",
           o1.data, o2.data);
        sb_free(&o1); sb_free(&o2); jj_free(a); jj_free(b); js_free(j);
    }

    /* 6. tool_calls survive the JSON -> Jinja conversion (serve used to
     *    drop every field except role and content) */
    {
        js_val *j = js_parse(
            "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":"
            "[{\"function\":{\"name\":\"f\"}}]}");
        jj_val *v = agent_js_to_jj(j);
        if (jj_dict_get(v, "tool_calls"))
            printf("ok   tool_calls round-trip into the template\n");
        else { printf("FAIL tool_calls dropped\n"); fails++; }
        jj_free(v); js_free(j);
    }

    /* 7. tool-call parsing, both the tagged and the bare form */
    {
        char *n = NULL; strbuf args;
        if (agent_parse_tool_call(
              "<tool_call><function=get><parameter=x>1</parameter>"
              "</function></tool_call>", &n, &args) &&
            strcmp(n,"get")==0 && strcmp(args.data,"{\"x\":1}")==0)
            printf("ok   tagged tool call parses\n");
        else { printf("FAIL tagged tool call\n"); fails++; }
        free(n); sb_free(&args);

        n = NULL;
        if (agent_parse_tool_call("<function=b><parameter=s>hi</parameter>",
                                  &n, &args) && strcmp(n,"b")==0)
            printf("ok   bare tool call parses\n");
        else { printf("FAIL bare tool call\n"); fails++; }
        free(n); sb_free(&args);
    }

    /* 8. all three tool-call shapes the model actually emits */
    {
        char *n = NULL; strbuf args;
        if (agent_parse_tool_call(
              "<tool_call>\n{\"name\": \"w\", \"arguments\": {\"city\": \"P\"}}\n"
              "</tool_call>", &n, &args) &&
            strcmp(n,"w")==0 && strcmp(args.data,"{\"city\":\"P\"}")==0)
            printf("ok   JSON tool call parses with its arguments\n");
        else { printf("FAIL JSON tool call\n"); fails++; }
        free(n); sb_free(&args);

        n = NULL;
        if (agent_parse_tool_call(
              "<tool_call>{\"function\":{\"name\":\"add\",\"arguments\":"
              "{\"a\":1}}}</tool_call>", &n, &args) &&
            strcmp(n,"add")==0 && strcmp(args.data,"{\"a\":1}")==0)
            printf("ok   OpenAI-nested JSON tool call parses\n");
        else { printf("FAIL nested JSON tool call\n"); fails++; }
        free(n); sb_free(&args);
    }

    /* 9. --think is one switch with an optional value, and never
     *    swallows the option that follows it. */
    {
        static char *a1[] = {"infer","chat","m.gguf","--think"};
        static char *a2[] = {"infer","chat","m.gguf","--think","off"};
        static char *a3[] = {"infer","chat","m.gguf","--think","--raw"};
        static char *a4[] = {"infer","chat","m.gguf"};
        infer_opts o;

        opts_parse(&o, 4, a1);
        if (o.thinking == 1) printf("ok   --think alone means on\n");
        else { printf("FAIL --think alone\n"); fails++; }

        opts_parse(&o, 5, a2);
        if (o.thinking == 0) printf("ok   --think off disables it\n");
        else { printf("FAIL --think off\n"); fails++; }

        opts_parse(&o, 5, a3);
        if (o.thinking == 1 && o.raw == 1)
            printf("ok   --think does not swallow the next option\n");
        else { printf("FAIL --think ate --raw\n"); fails++; }

        opts_parse(&o, 3, a4);
        if (o.thinking == 0) printf("ok   thinking defaults to off\n");
        else { printf("FAIL default thinking\n"); fails++; }

        /* --no-think must be gone, not merely undocumented */
        {
            static char *a5[] = {"infer","chat","m.gguf","--no-think"};
            if (opts_parse(&o, 4, a5) != 0)
                printf("ok   --no-think removed\n");
            else { printf("FAIL --no-think still parses\n"); fails++; }
        }
    }

    /* 10. Logging is opt-in. This is the regression that mattered:
     *     a -DINFER_PROFILE build used to force it on. */
    {
        static char *a1[] = {"infer","run","m.gguf"};
        static char *a2[] = {"infer","run","m.gguf","--log-perf"};
        infer_opts o;

        opts_parse(&o, 3, a1);
        if (!o.log_perf && !o.log_stages)
            printf("ok   no logging is requested by default\n");
        else { printf("FAIL logging on by default\n"); fails++; }

        opts_parse(&o, 4, a2);
        if (o.log_perf) printf("ok   --log-perf requests logging\n");
        else { printf("FAIL --log-perf\n"); fails++; }

        /* the report itself must respect the switch */
        {
            perf_run r;
            perf_total t;
            prof_perf_enabled = 0;
            perf_total_init(&t);
            perf_begin(&r);
            perf_mark_prompt_done(&r, 5);
            perf_end(&r, 5);
            perf_total_add(&t, &r);
            if (t.n_turns == 0)
                printf("ok   perf totals stay empty while disabled\n");
            else { printf("FAIL perf recorded while disabled\n"); fails++; }
        }
    }

    jj_free(m);
    printf(fails ? "\n%d FAILURE(S)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
