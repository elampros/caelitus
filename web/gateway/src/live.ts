/**
 * Bridges MQTT to browsers: catalog events, incoming likes/dislikes and the
 * server's online/offline status are pushed to every WebSocket client as
 * small JSON messages. Also publishes likes/dislikes coming from the UI.
 *
 * @module
 */

import mqtt, { type MqttClient } from "mqtt";

/** One message pushed to WebSocket clients; `at` is when the gateway saw it (ISO 8601). */
export type LiveMessage =
  | { type: "reaction"; bookId: number; kind: "like" | "dislike"; at: string }
  | { type: "event"; bookId: number; event: string; payload: string; at: string }
  | { type: "status"; server: string; at: string }
  | { type: "broker"; connected: boolean; at: string };

/** Where the broker is and which topics to follow. */
export interface LiveOptions {
  /** Broker URL, e.g. `mqtt://127.0.0.1:1883`. */
  url: string;
  /** Prefix of like/dislike topics, e.g. `catalog/in/books`. */
  reactionPrefix: string;
  /** Prefix of catalog event topics, e.g. `catalog/books`. */
  eventPrefix: string;
  /** The server's online/offline topic, e.g. `caelitus/status`. */
  statusTopic: string;
}

/**
 * One MQTT connection shared by every browser: subscribes once and fans each
 * message out to the listeners. Reconnects on its own every 2 seconds.
 */
export class Live {
  private readonly client: MqttClient;
  private readonly listeners = new Set<(m: LiveMessage) => void>();
  private lastStatus: LiveMessage | null = null;

  /** Connects to the broker; subscriptions are (re)made on every connect. */
  constructor(private readonly options: LiveOptions) {
    this.client = mqtt.connect(options.url, {
      clientId: `caelitus-gateway-${process.pid}`,
      reconnectPeriod: 2000,
    });
    this.client.on("connect", () => {
      this.client.subscribe([
        `${options.eventPrefix}/+/+`,
        `${options.reactionPrefix}/+/like`,
        `${options.reactionPrefix}/+/dislike`,
        options.statusTopic,
      ]);
      this.emit({ type: "broker", connected: true, at: new Date().toISOString() });
    });
    this.client.on("close", () => this.emit({ type: "broker", connected: false, at: new Date().toISOString() }));
    this.client.on("error", () => {});  // reconnects on its own; state is reported via "close"
    this.client.on("message", (topic, payload) => this.onMessage(topic, payload.toString("utf8")));
  }

  /** Whether the broker connection is up right now. */
  get brokerConnected(): boolean {
    return this.client.connected;
  }

  /**
   * The latest status message, sent to clients as soon as they connect.
   */
  get status(): LiveMessage | null {
    return this.lastStatus;
  }

  /**
   * Calls `listener` with every message from now on.
   * @returns A function that unsubscribes.
   */
  subscribe(listener: (m: LiveMessage) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  /**
   * A like/dislike from the UI travels the same way as one from a device.
   * @returns false if the broker is not connected (nothing was sent).
   */
  react(bookId: number, kind: "like" | "dislike"): boolean {
    if (!this.client.connected) return false;
    this.client.publish(`${this.options.reactionPrefix}/${bookId}/${kind}`, "");
    return true;
  }

  /** Disconnects from the broker. */
  close(): Promise<void> {
    return new Promise((resolve) => this.client.end(false, {}, () => resolve()));
  }

  private onMessage(topic: string, payload: string): void {
    const at = new Date().toISOString();
    if (topic === this.options.statusTopic) {
      this.lastStatus = { type: "status", server: payload, at };
      return this.emit(this.lastStatus);
    }
    const reaction = this.match(topic, this.options.reactionPrefix);
    if (reaction && (reaction.last === "like" || reaction.last === "dislike"))
      return this.emit({ type: "reaction", bookId: reaction.id, kind: reaction.last, at });
    const event = this.match(topic, this.options.eventPrefix);
    if (event) this.emit({ type: "event", bookId: event.id, event: event.last, payload, at });
  }

  /**
   * "<prefix>/<id>/<last>" -> { id, last }
   */
  private match(topic: string, prefix: string): { id: number; last: string } | null {
    if (!topic.startsWith(prefix + "/")) return null;
    const [idText, last, ...rest] = topic.slice(prefix.length + 1).split("/");
    const id = Number(idText);
    if (rest.length || !last || !Number.isSafeInteger(id) || id <= 0) return null;
    return { id, last };
  }

  private emit(message: LiveMessage): void {
    for (const listener of this.listeners) listener(message);
  }
}
