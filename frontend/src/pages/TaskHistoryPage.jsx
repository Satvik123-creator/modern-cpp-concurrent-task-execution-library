import React, { useState } from 'react';
import { submitTask, submitTaskPriority, scheduleTask, cancelTask } from '../services/api';
import StatusBadge from '../components/StatusBadge';
import { usePolling } from '../hooks/usePolling';
import { getMetrics } from '../services/api';
import LoadingSpinner from '../components/LoadingSpinner';

const PRIORITY_COLORS = {
  critical: 'text-red-500 font-bold',
  high: 'text-orange-500 font-semibold',
  normal: 'text-gray-600 dark:text-gray-300',
  low: 'text-gray-400',
};

export default function TaskHistoryPage() {
  const [taskType, setTaskType] = useState('noop');
  const [name, setName] = useState('');
  const [priority, setPriority] = useState('normal');
  const [durationMs, setDurationMs] = useState(100);
  const [iterations, setIterations] = useState(1000000);
  const [delayMs, setDelayMs] = useState(0);
  const [mode, setMode] = useState('immediate');
  const [submitting, setSubmitting] = useState(false);
  const [msg, setMsg] = useState(null);
  const [filter, setFilter] = useState('all');

  const { data: metrics, loading } = usePolling(() => getMetrics(), 2000);

  const history = metrics?.history ?? [];
  const recentTasks = history.slice(-50).reverse();

  const filtered = filter === 'all' ? recentTasks : recentTasks.filter((t) => t.status === filter);

  const handleSubmit = async (e) => {
    e.preventDefault();
    setSubmitting(true);
    setMsg(null);
    const payload = { task_type: taskType, name: name || undefined, priority };
    if (taskType === 'sleep') payload.duration_ms = parseInt(durationMs, 10);
    if (taskType === 'compute') payload.iterations = parseInt(iterations, 10);

    try {
      let res;
      if (mode === 'scheduled') {
        payload.delay_ms = parseInt(delayMs, 10);
        res = await scheduleTask(payload);
      } else if (mode === 'priority') {
        res = await submitTaskPriority(payload);
      } else {
        res = await submitTask(payload);
      }
      setMsg({ type: 'success', text: `Task ${res.data.task_id?.slice(0, 8) || 'submitted'}...` });
    } catch (err) {
      setMsg({ type: 'error', text: err.message });
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <div className="space-y-6">
      <h2 className="text-2xl font-bold">Task History</h2>

      {/* Submit form */}
      <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
        <h3 className="font-semibold mb-4">Submit Task</h3>
        <form onSubmit={handleSubmit} className="space-y-4">
          <div className="flex gap-2">
            {['immediate', 'priority', 'scheduled'].map((m) => (
              <button key={m} type="button" onClick={() => setMode(m)}
                className={`px-3 py-1.5 rounded-lg text-sm font-medium transition-colors ${
                  mode === m ? 'bg-blue-600 text-white' : 'bg-gray-100 dark:bg-gray-800 text-gray-500 hover:bg-gray-200 dark:hover:bg-gray-700'
                }`}>
                {m.charAt(0).toUpperCase() + m.slice(1)}
              </button>
            ))}
          </div>

          <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
            <div>
              <label className="block text-xs text-gray-500 mb-1">Type</label>
              <select value={taskType} onChange={(e) => setTaskType(e.target.value)}
                className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm">
                <option value="noop">No-Op</option>
                <option value="compute">Compute</option>
                <option value="sleep">Sleep</option>
              </select>
            </div>
            <div>
              <label className="block text-xs text-gray-500 mb-1">Priority</label>
              <select value={priority} onChange={(e) => setPriority(e.target.value)}
                className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm">
                <option value="low">Low</option>
                <option value="normal">Normal</option>
                <option value="high">High</option>
                <option value="critical">Critical</option>
              </select>
            </div>
            {taskType === 'sleep' && (
              <div>
                <label className="block text-xs text-gray-500 mb-1">Duration (ms)</label>
                <input type="number" value={durationMs} onChange={(e) => setDurationMs(e.target.value)} min="1"
                  className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm" />
              </div>
            )}
            {taskType === 'compute' && (
              <div>
                <label className="block text-xs text-gray-500 mb-1">Iterations</label>
                <input type="number" value={iterations} onChange={(e) => setIterations(e.target.value)} min="1"
                  className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm" />
              </div>
            )}
            {mode === 'scheduled' && (
              <div>
                <label className="block text-xs text-gray-500 mb-1">Delay (ms)</label>
                <input type="number" value={delayMs} onChange={(e) => setDelayMs(e.target.value)} min="0"
                  className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm" />
              </div>
            )}
            <div>
              <label className="block text-xs text-gray-500 mb-1">Name</label>
              <input type="text" value={name} onChange={(e) => setName(e.target.value)}
                className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm"
                placeholder="Optional..." />
            </div>
          </div>

          <div className="flex items-center gap-3">
            <button type="submit" disabled={submitting}
              className="px-5 py-2 bg-blue-600 hover:bg-blue-700 disabled:bg-gray-400 text-white rounded-lg text-sm font-medium transition-colors">
              {submitting ? 'Submitting...' : 'Submit'}
            </button>
            {msg && (
              <span className={`text-sm ${msg.type === 'success' ? 'text-green-500' : 'text-red-500'}`}>{msg.text}</span>
            )}
          </div>
        </form>
      </div>

      {/* History */}
      <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800">
        <div className="px-5 py-4 border-b border-gray-200 dark:border-gray-800 flex items-center justify-between">
          <h3 className="font-semibold">Recent Activity</h3>
          <div className="flex gap-1">
            {['all', 'completed', 'failed', 'cancelled'].map((f) => (
              <button key={f} onClick={() => setFilter(f)}
                className={`px-2 py-1 rounded text-xs font-medium ${
                  filter === f ? 'bg-blue-600 text-white' : 'bg-gray-100 dark:bg-gray-800 text-gray-500'
                }`}>
                {f}
              </button>
            ))}
          </div>
        </div>
        <div className="divide-y divide-gray-200 dark:divide-gray-800 max-h-96 overflow-y-auto">
          {loading ? (
            <LoadingSpinner size="sm" text="" />
          ) : filtered.length === 0 ? (
            <p className="p-5 text-gray-400 text-center text-sm">No tasks recorded yet</p>
          ) : (
            filtered.map((t, i) => (
              <div key={i} className="px-5 py-3 flex items-center justify-between hover:bg-gray-50 dark:hover:bg-gray-800/50">
                <div className="flex items-center gap-3">
                  <span className="text-sm font-mono text-gray-400 w-8">#{i + 1}</span>
                  <div>
                    <p className="text-sm font-medium">{t.name || 'task'}</p>
                    <p className="text-xs text-gray-400">
                      threads: {t.active_threads ?? 0} · queue: {t.queue_length ?? 0}
                    </p>
                  </div>
                </div>
                <span className="text-xs text-gray-400">
                  {(t.tasks_per_sec ?? 0).toFixed(1)} t/s
                </span>
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  );
}
