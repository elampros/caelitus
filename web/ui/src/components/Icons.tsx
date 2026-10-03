// Small line icons (24×24, stroke = currentColor), so they follow the text
// color in both themes. Decorative by default: give a label where they stand alone.

import type { ReactNode, SVGProps } from "react";

type IconProps = SVGProps<SVGSVGElement> & { size?: number };

function icon(paths: ReactNode) {
  return function Icon({ size = 16, ...rest }: IconProps) {
    return (
      <svg
        width={size}
        height={size}
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        strokeWidth={2}
        strokeLinecap="round"
        strokeLinejoin="round"
        aria-hidden="true"
        {...rest}
      >
        {paths}
      </svg>
    );
  };
}

// Thumb shapes after Lucide (https://lucide.dev, ISC license).
export const ThumbUp = icon(
  <>
    <path d="M7 10v12" />
    <path d="M15 5.88 14 10h5.83a2 2 0 0 1 1.92 2.56l-2.33 8A2 2 0 0 1 17.5 22H4a2 2 0 0 1-2-2v-8a2 2 0 0 1 2-2h2.76a2 2 0 0 0 1.79-1.11L12 2a3.13 3.13 0 0 1 3 3.88Z" />
  </>,
);
export const ThumbDown = icon(
  <>
    <path d="M17 14V2" />
    <path d="M9 18.12 10 14H4.17a2 2 0 0 1-1.92-2.56l2.33-8A2 2 0 0 1 6.5 2H20a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-2.76a2 2 0 0 0-1.79 1.11L12 22a3.13 3.13 0 0 1-3-3.88Z" />
  </>,
);
export const Star = icon(<path d="m12 3 2.8 5.7 6.2.9-4.5 4.4 1.1 6.2L12 17.3 6.4 20.2l1.1-6.2L3 9.6l6.2-.9z" />);
export const Search = icon(
  <>
    <circle cx="11" cy="11" r="7" />
    <path d="m20 20-3.5-3.5" />
  </>,
);
export const Close = icon(<path d="M18 6 6 18M6 6l12 12" />);
export const ChevronLeft = icon(<path d="m15 18-6-6 6-6" />);
export const ChevronRight = icon(<path d="m9 18 6-6-6-6" />);
export const BookIcon = icon(
  <>
    <path d="M4 19.5V5a2 2 0 0 1 2-2h13v16H6.5A2.5 2.5 0 0 0 4 21.5" />
    <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H19" />
  </>,
);
export const Pulse = icon(<path d="M3 12h4l3-8 4 16 3-8h4" />);
export const Code = icon(<path d="m8 6-6 6 6 6M16 6l6 6-6 6" />);
export const Server = icon(
  <>
    <rect x="3" y="4" width="18" height="7" rx="2" />
    <rect x="3" y="13" width="18" height="7" rx="2" />
    <path d="M7 7.5h.01M7 16.5h.01" />
  </>,
);
export const Alert = icon(
  <>
    <path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z" />
    <path d="M12 9v4M12 17h.01" />
  </>,
);
export const Filter = icon(<path d="M3 5h18l-7 8v6l-4 2v-8z" />);
export const Library = icon(
  <>
    <path d="M4 4v16M8 4v16" />
    <path d="m12 4.5 4 15.5M16.2 4l3.8 15" />
  </>,
);
