import React from 'react';

export default function ErrorAlert({ message, onRetry }) {
  if (!message) return null;
  return (
    <div className="bg-red-900/50 border border-red-700 rounded-lg p-4 flex items-start gap-3">
      <svg className="w-5 h-5 text-red-400 mt-0.5 shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 9v2m0 4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
      </svg>
      <div className="flex-1">
        <p className="text-red-300 text-sm">{message}</p>
      </div>
      {onRetry && (
        <button onClick={onRetry} className="text-sm text-red-400 hover:text-red-300 underline shrink-0">
          Retry
        </button>
      )}
    </div>
  );
}
