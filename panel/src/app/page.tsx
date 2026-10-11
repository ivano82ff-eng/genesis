"use client";

import { useState } from "react";

type Phase = "empty" | "loading" | "error" | "ready";

type BootResponse = {
  ok?: boolean;
  log?: string;
  error?: string;
  image?: string;
};

export default function Home() {
  const [phase, setPhase] = useState<Phase>("empty");
  const [log, setLog] = useState("");
  const [image, setImage] = useState("");
  const [error, setError] = useState("");

  async function launch(): Promise<void> {
    setPhase("loading");
    setError("");
    try {
      const response = await fetch("/api/boot", { method: "POST", cache: "no-store" });
      const body = (await response.json()) as BootResponse;
      setLog(body.log ?? "");
      setImage(body.image ?? "");
      if (!response.ok || !body.ok) {
        setError(body.error || "Не удалось получить журнал загрузки");
        setPhase("error");
        return;
      }
      setPhase("ready");
    } catch {
      setError("Панель не дождалась ответа сервера");
      setPhase("error");
    }
  }

  return (
    <main className="mx-auto flex w-full max-w-3xl flex-1 flex-col gap-6 px-4 py-8 sm:px-6 sm:py-12">
      <header className="flex flex-col gap-2">
        <p className="text-sm text-muted-foreground">x86_64 · QEMU · COM1</p>
        <h1 className="text-2xl font-semibold tracking-tight sm:text-3xl">
          Журнал загрузки genesis
        </h1>
        <p className="max-w-2xl text-sm leading-6 text-muted-foreground sm:text-base">
          Кнопка собирает образ, поднимает гостя и показывает домашний экран. Щелчок по
          «Память», «Файлы», «Редактор» или «Открыть» открывает один раздел, «Назад»
          возвращает домой.
        </p>
      </header>

      <button
        type="button"
        className="inline-flex h-9 w-full items-center justify-center rounded-lg bg-primary px-3 text-sm font-medium text-primary-foreground transition-colors hover:bg-primary/80 disabled:pointer-events-none disabled:opacity-50 sm:w-fit"
        disabled={phase === "loading"}
        aria-busy={phase === "loading"}
        onClick={() => {
          void launch();
        }}
      >
        Запустить
      </button>

      <section
        aria-live="polite"
        className="min-h-48 rounded-xl border bg-card p-4 text-card-foreground"
      >
        {phase === "empty" ? (
          <p className="text-sm text-muted-foreground">
            Экран пуст. Нажмите «Запустить», чтобы увидеть домашний экран.
          </p>
        ) : null}
        {phase === "loading" ? (
          <p className="text-sm">Собираем образ и рисуем домашний экран…</p>
        ) : null}
        {phase === "error" ? (
          <div className="flex flex-col gap-3">
            <p className="text-sm whitespace-pre-wrap text-destructive">{error}</p>
            {log !== "" ? <Log text={log} /> : null}
          </div>
        ) : null}
        {phase === "ready" ? (
          <div className="flex flex-col gap-3">
            {image !== "" ? (
              <img
                alt="Домашний экран genesis"
                src={`data:image/png;base64,${image}`}
                className="h-auto w-full rounded-lg border"
              />
            ) : null}
            <p className="text-sm text-muted-foreground">
              Окно QEMU остаётся открытым, пока вы его не закроете. Снимок на странице не
              пропадает.
            </p>
            <Log text={log} />
          </div>
        ) : null}
      </section>
    </main>
  );
}

function Log({ text }: { text: string }) {
  return (
    <pre className="overflow-x-auto font-mono text-sm leading-6 break-words whitespace-pre-wrap">
      {text}
    </pre>
  );
}
