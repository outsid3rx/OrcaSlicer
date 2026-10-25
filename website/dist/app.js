"use strict";
// URLs choose the language. Fresh visits to the site root always use English.
const russian = document.documentElement.lang === "ru";
const ui = russian
  ? {
      promptCopied: "Промпт скопирован. Вставьте его в чат с агентом.",
      commandsCopied: "Команды скопированы.",
      copyUnavailable:
        "Буфер обмена недоступен. Текст выделен — скопируйте его вручную.",
      modelAlt:
        "Реальный предпросмотр из OrcaSlicer: два тестовых куба на столе",
      sliceAlt:
        "Реальный предпросмотр из OrcaSlicer: траектории печати двух тестовых кубов",
      modelLabel: "Модель на столе",
      sliceLabel: "Траектории печати",
      modelStatus: "Модель",
      sliceStatus: "Нарезка",
    }
  : {
      promptCopied: "Prompt copied. Paste it into your agent's chat.",
      commandsCopied: "Commands copied.",
      copyUnavailable:
        "Clipboard unavailable. The text is selected — copy it manually.",
      modelAlt: "Actual OrcaSlicer preview: two test cubes on a plate",
      sliceAlt: "Actual OrcaSlicer preview: toolpaths for two test cubes",
      modelLabel: "Model on plate",
      sliceLabel: "Print toolpaths",
      modelStatus: "Model",
      sliceStatus: "Slice",
    };
let messageTimer;
const status = document.getElementById("copy-status");
function announce(text) {
  clearTimeout(messageTimer);
  status.textContent = text;
  status.classList.add("visible");
  messageTimer = setTimeout(() => status.classList.remove("visible"), 4500);
}
async function copyText(text) {
  if (navigator.clipboard && window.isSecureContext) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (_) {
      /* Try local selection fallback. */
    }
  }
  const area = document.createElement("textarea");
  area.value = text;
  area.setAttribute("readonly", "");
  area.style.position = "fixed";
  area.style.top = "-9999px";
  document.body.append(area);
  area.select();
  let result = false;
  try {
    result = document.execCommand("copy");
  } catch (_) {
    result = false;
  }
  area.remove();
  return result;
}
document.querySelectorAll("[data-copy]").forEach((button) =>
  button.addEventListener("click", async () => {
    const target = document.getElementById(button.dataset.copy);
    const active = document.activeElement;
    if (await copyText(target.textContent)) {
      active?.focus({ preventScroll: true });
      announce(
        button.dataset.copy === "install-prompt"
          ? ui.promptCopied
          : ui.commandsCopied,
      );
    } else {
      const selection = window.getSelection();
      const range = document.createRange();
      range.selectNodeContents(target);
      selection.removeAllRanges();
      selection.addRange(range);
      target.focus({ preventScroll: true });
      announce(ui.copyUnavailable);
    }
  }),
);
const assetPrefix = russian ? "../assets/" : "assets/";
const views = {
  model: {
    src: assetPrefix + "model.png",
    alt: ui.modelAlt,
    label: ui.modelLabel,
    status: ui.modelStatus,
  },
  slice: {
    src: assetPrefix + "slice.png",
    alt: ui.sliceAlt,
    label: ui.sliceLabel,
    status: ui.sliceStatus,
  },
};
document.querySelectorAll("[data-view]").forEach((button) =>
  button.addEventListener("click", () => {
    const view = views[button.dataset.view];
    const image = document.getElementById("native-image");
    image.src = view.src;
    image.alt = view.alt;
    document.getElementById("image-tag").textContent = view.label;
    document.getElementById("view-status").textContent = view.status;
    document.querySelectorAll("[data-view]").forEach((tab) => {
      const active = tab === button;
      tab.classList.toggle("active", active);
      tab.setAttribute("aria-pressed", String(active));
    });
  }),
);
function openManual() {
  if (location.hash === "#manual")
    document.getElementById("manual").open = true;
}
window.addEventListener("hashchange", openManual);
openManual();
