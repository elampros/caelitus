// Generated from docs/openrpc.json (Caelitus Catalog API 1.0.0) by web/scripts/gen-types.ts.
// Do not edit: run `npm run gen` instead.

export type Author = {
  bio: string | null;
  birthDate: string | null;
  createdAt: string;
  /** Positive integer id */
  id: number;
  name: string;
  updatedAt: string;
  /** Pass it back to authors.update */
  version: number;
};

export type AuthorPage = {
  items: Author[];
  page: number;
  pageCount: number;
  pageSize: number;
  /** Matching items across all pages */
  total: number;
};

export type AuthorRef = {
  /** Positive integer id */
  id: number;
  name: string;
};

/** A book with all its details */
export type Book = {
  authors: AuthorRef[];
  category: Category;
  createdAt: string;
  description: string | null;
  dislikes: number;
  /** Positive integer id */
  id: number;
  isbn: string | null;
  language: string;
  likes: number;
  pageCount: number | null;
  publishedOn: string;
  ratingAverage: number | null;
  ratingCount: number;
  /** Whether likes/dislikes are accepted over MQTT */
  reactionsEnabled: boolean;
  tags: string[];
  title: string;
  updatedAt: string;
  /** Pass it back to books.update */
  version: number;
};

export type BookPage = {
  items: BookSummary[];
  page: number;
  pageCount: number;
  pageSize: number;
  /** Matching items across all pages */
  total: number;
};

/** A book as listed in search results */
export type BookSummary = {
  authors: AuthorRef[];
  category: Category;
  dislikes: number;
  /** Positive integer id */
  id: number;
  language: string;
  likes: number;
  publishedOn: string;
  ratingAverage: number | null;
  ratingCount: number;
  /** Whether likes/dislikes are accepted over MQTT */
  reactionsEnabled: boolean;
  tags: string[];
  title: string;
};

export type Category = {
  /** Positive integer id */
  id: number;
  name: string;
  /** a-z, 0-9 and '-' */
  slug: string;
};

export type RankedBook = {
  /** Positive integer id */
  bookId: number;
  dislikes: number;
  likes: number;
  score: number;
  title: string;
};

export type ReactionCounts = {
  dislikes: number;
  likes: number;
  /** likes - dislikes */
  score: number;
};

export type ReactionStats = {
  /** Positive integer id */
  bookId: number;
  periods: {
    allTime: ReactionCounts;
    last30Days: ReactionCounts;
    last7Days: ReactionCounts;
    lastYear: ReactionCounts;
    today: ReactionCounts;
    yesterday: ReactionCounts;
  };
};

export type Review = {
  body: string;
  /** Positive integer id */
  bookId: number;
  createdAt: string;
  /** Positive integer id */
  id: number;
  rating: number;
  reviewerName: string;
  title: string | null;
  updatedAt: string;
};

export type ReviewPage = {
  items: Review[];
  page: number;
  pageCount: number;
  pageSize: number;
  /** Matching items across all pages */
  total: number;
};

export type TagUsage = {
  bookCount: number;
  name: string;
};

export type TopBooks = {
  /** First day; null for allTime */
  from: string | null;
  items: RankedBook[];
  period: "today" | "yesterday" | "last7Days" | "last30Days" | "lastYear" | "allTime";
  /** Last day; null for allTime */
  to: string | null;
};

export interface Methods {
  /** Creates an author. */
  "authors.create": {
    params: {
      /** Full name */
      name: string;
      /** Biography */
      bio?: string | null;
      /** Not in the future */
      birthDate?: string | null;
    };
    result: Author;
  };
  /** Deletes an author who has no books. */
  "authors.delete": {
    params: {
      /** Author id */
      id: number;
    };
    result: true;
  };
  /** One author. */
  "authors.get": {
    params: {
      /** Author id */
      id: number;
    };
    result: Author;
  };
  /** Authors by name, alphabetically. */
  "authors.search": {
    params: {
      /** Part of the name (case-insensitive) */
      name?: string | null;
      /** Page number, from 1 (default 1) */
      page?: number | null;
      /** Items per page, 1-100 (default 20) */
      pageSize?: number | null;
    };
    result: AuthorPage;
  };
  /** Replaces an author's fields. */
  "authors.update": {
    params: {
      /** Author id */
      id: number;
      /** The version you last read */
      version: number;
      /** Full name */
      name: string;
      /** Biography; omitted or null clears it */
      bio?: string | null;
      /** Omitted or null clears it */
      birthDate?: string | null;
    };
    result: Author;
  };
  /** Creates a book. */
  "books.create": {
    params: {
      /** Title */
      title: string;
      /** ISBN-10 or ISBN-13; hyphens and spaces allowed; stored as ISBN-13 */
      isbn?: string | null;
      /** Free text */
      description?: string | null;
      /** Publication date */
      publishedOn: string;
      /** ISO 639-1 language code, e.g. "el", "en" */
      language: string;
      /** Number of pages */
      pageCount?: number | null;
      /** The book's category */
      categoryId: number;
      /** Authors in cover order */
      authorIds: number[];
      /** Free-text tags; normalized (trimmed, lower case); unknown tags are created */
      tags?: string[] | null;
      /** Accept likes/dislikes over MQTT (default false) */
      reactionsEnabled?: boolean | null;
    };
    result: Book;
  };
  /** Deletes a book with its reviews and reactions. */
  "books.delete": {
    params: {
      /** Book id */
      id: number;
    };
    result: true;
  };
  /** One book with all its details. */
  "books.get": {
    params: {
      /** Book id */
      id: number;
    };
    result: Book;
  };
  /** Searches books. */
  "books.search": {
    params: {
      /** Only this category */
      categoryId?: number | null;
      /** Only books by this author */
      authorId?: number | null;
      /** Tag names */
      tags?: string[] | null;
      /** any (default): at least one tag; all: every tag */
      tagMatch?: "any" | "all" | null;
      /** Published on or after */
      publishedFrom?: string | null;
      /** Published on or before */
      publishedTo?: string | null;
      /** Part of the title (case-insensitive) */
      title?: string | null;
      /** Minimum average rating; unrated books are excluded */
      minRating?: number | null;
      /** ISO 639-1 code */
      language?: string | null;
      /** Order (default publishedDesc); ratingDesc puts unrated last */
      sort?: "publishedDesc" | "publishedAsc" | "titleAsc" | "ratingDesc" | "createdDesc" | null;
      /** Page number, from 1 (default 1) */
      page?: number | null;
      /** Items per page, 1-100 (default 20) */
      pageSize?: number | null;
    };
    result: BookPage;
  };
  /** Turns likes/dislikes over MQTT on or off for a book. */
  "books.setReactionsEnabled": {
    params: {
      /** Book id */
      id: number;
      /** true to accept likes/dislikes */
      enabled: boolean;
    };
    result: Book;
  };
  /** Replaces a book's fields, authors and tags. */
  "books.update": {
    params: {
      /** Book id */
      id: number;
      /** The version you last read */
      version: number;
      /** Title */
      title: string;
      /** ISBN-10 or ISBN-13; hyphens and spaces allowed; stored as ISBN-13 */
      isbn?: string | null;
      /** Free text */
      description?: string | null;
      /** Publication date */
      publishedOn: string;
      /** ISO 639-1 language code, e.g. "el", "en" */
      language: string;
      /** Number of pages */
      pageCount?: number | null;
      /** The book's category */
      categoryId: number;
      /** Authors in cover order */
      authorIds: number[];
      /** Free-text tags; normalized (trimmed, lower case); unknown tags are created */
      tags?: string[] | null;
    };
    result: Book;
  };
  /** Creates a category. */
  "categories.create": {
    params: {
      /** Unique name */
      name: string;
      /** Unique URL-friendly id (a-z, 0-9, '-'); derived from the name if omitted, which needs a name with latin letters or digits */
      slug?: string | null;
    };
    result: Category;
  };
  /** Deletes a category that has no books. */
  "categories.delete": {
    params: {
      /** Category id */
      id: number;
    };
    result: true;
  };
  /** One category. */
  "categories.get": {
    params: {
      /** Category id */
      id: number;
    };
    result: Category;
  };
  /** All categories, by name. */
  "categories.list": {
    params: Record<string, never>;
    result: Category[];
  };
  /** Renames a category. */
  "categories.update": {
    params: {
      /** Category id */
      id: number;
      /** New name */
      name: string;
      /** New slug; kept if omitted */
      slug?: string | null;
    };
    result: Category;
  };
  /** A book's likes and dislikes for every period. */
  "reactions.get": {
    params: {
      /** Book id */
      bookId: number;
    };
    result: ReactionStats;
  };
  /** Most liked (or disliked) books in a period. */
  "reactions.top": {
    params: {
      /** Time window */
      period: "today" | "yesterday" | "last7Days" | "last30Days" | "lastYear" | "allTime";
      /** mostLiked (default) or mostDisliked */
      order?: "mostLiked" | "mostDisliked" | null;
      /** How many books, 1-100 (default 10) */
      limit?: number | null;
    };
    result: TopBooks;
  };
  /** Adds a review and updates the book's rating. */
  "reviews.create": {
    params: {
      /** Book id */
      bookId: number;
      /** Who wrote the review */
      reviewerName: string;
      /** 1 (worst) to 5 (best) */
      rating: number;
      /** Optional headline */
      title?: string | null;
      /** The review text */
      body: string;
    };
    result: Review;
  };
  /** Deletes a review and updates the book's rating. */
  "reviews.delete": {
    params: {
      /** Review id */
      id: number;
    };
    result: true;
  };
  /** One review. */
  "reviews.get": {
    params: {
      /** Review id */
      id: number;
    };
    result: Review;
  };
  /** A book's reviews, newest first. */
  "reviews.list": {
    params: {
      /** Book id */
      bookId: number;
      /** Page number, from 1 (default 1) */
      page?: number | null;
      /** Items per page, 1-100 (default 20) */
      pageSize?: number | null;
    };
    result: ReviewPage;
  };
  /** Replaces a review's fields and updates the book's rating. */
  "reviews.update": {
    params: {
      /** Review id */
      id: number;
      /** Who wrote the review */
      reviewerName: string;
      /** 1 (worst) to 5 (best) */
      rating: number;
      /** Optional headline */
      title?: string | null;
      /** The review text */
      body: string;
    };
    result: Review;
  };
  /** Returns this API's OpenRPC description. */
  "rpc.discover": {
    params: Record<string, never>;
    result: Record<string, unknown>;
  };
  /** Liveness check. */
  "system.ping": {
    params: Record<string, never>;
    result: {
      /** Server time */
      time: string;
    };
  };
  /** Tags in use, by name, with how many books carry each. */
  "tags.list": {
    params: Record<string, never>;
    result: TagUsage[];
  };
}

export type MethodName = keyof Methods;
