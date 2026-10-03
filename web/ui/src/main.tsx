/**
 * The UI's entry point: the layout (navigation, status indicators, theme
 * switch) and the routes of the three pages: `/` books, `/live` dashboard,
 * `/api` method reference. Rendered into `#root` of `index.html`.
 *
 * @module
 */

import { StrictMode, useEffect, useState } from "react";
import { createRoot } from "react-dom/client";
import { BrowserRouter, NavLink, Navigate, Route, Routes } from "react-router-dom";
import { useLiveState } from "./api/live";
import { ApiPage } from "./pages/ApiPage";
import { BooksPage } from "./pages/BooksPage";
import { DashboardPage } from "./pages/DashboardPage";
import "@fontsource-variable/inter";
import { BookIcon, Code, Library, Pulse } from "./components/Icons";
import "./styles.css";

function StatusPills() {
  const live = useLiveState();
  const server =
    live.server === "online" ? ["good", "Server online"] : live.server === "offline" ? ["bad", "Server offline"] : ["wait", "Server —"];
  const broker = live.broker ? ["good", "MQTT"] : live.broker === false ? ["bad", "MQTT εκτός"] : ["wait", "MQTT —"];
  const socket = live.socket === "open" ? ["good", "Live"] : ["wait", "Σύνδεση…"];
  return (
    <div className="status-group" aria-label="Κατάσταση συστήματος">
      {[server, broker, socket].map(([kind, label]) => (
        <span className="pill" key={label}>
          <span className={`dot ${kind}`} aria-hidden="true" />
          {label}
        </span>
      ))}
    </div>
  );
}

function ThemeToggle() {
  const [theme, setTheme] = useState<string | null>(() => {
    try {
      return localStorage.getItem("theme");
    } catch {
      return null;
    }
  });
  useEffect(() => {
    if (theme) document.documentElement.dataset.theme = theme;
    else delete document.documentElement.dataset.theme;
    try {
      if (theme) localStorage.setItem("theme", theme);
      else localStorage.removeItem("theme");
    } catch {
      /* storage unavailable: the choice lasts for this visit */
    }
  }, [theme]);
  const next = theme === null ? "dark" : theme === "dark" ? "light" : null;
  const label = theme === null ? "Θέμα: αυτόματο" : theme === "dark" ? "Θέμα: σκοτεινό" : "Θέμα: φωτεινό";
  return (
    <button className="ghost" onClick={() => setTheme(next)} title={`${label} (αλλαγή)`} aria-label={label} style={{ padding: "6px 8px", lineHeight: 0 }}>
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
        {theme === "light" ? (
          <>
            <circle cx="12" cy="12" r="4" />
            <path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4" />
          </>
        ) : theme === "dark" ? (
          <path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z" />
        ) : (
          <>
            <circle cx="12" cy="12" r="9" />
            <path d="M12 3a9 9 0 0 1 0 18z" fill="currentColor" />
          </>
        )}
      </svg>
    </button>
  );
}

function App() {
  return (
    <div className="app">
      <header className="topbar">
        <NavLink to="/books" className="brand">
          <span className="brand-mark" aria-hidden="true">
            <Library size={16} />
          </span>
          Caelitus
        </NavLink>
        <nav className="nav">
          <NavLink to="/books"><BookIcon />Βιβλία</NavLink>
          <NavLink to="/live"><Pulse />Live</NavLink>
          <NavLink to="/api"><Code />API</NavLink>
        </nav>
        <div className="topbar-right">
          <StatusPills />
          <ThemeToggle />
        </div>
      </header>
      <main className="main">
        <Routes>
          <Route path="/books" element={<BooksPage />} />
          <Route path="/live" element={<DashboardPage />} />
          <Route path="/api" element={<ApiPage />} />
          <Route path="*" element={<Navigate to="/books" replace />} />
        </Routes>
      </main>
    </div>
  );
}

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <BrowserRouter>
      <App />
    </BrowserRouter>
  </StrictMode>,
);
