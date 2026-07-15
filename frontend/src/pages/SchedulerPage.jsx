import React, { useState } from 'react';
import { usePolling } from '../hooks/usePolling';
import { scheduleTask } from '../services/api';
import LoadingSpinner from '../components/LoadingSpinner';
import ErrorAlert from '../components/ErrorAlert';

export default function SchedulerPage() {
  const [taskType, setTaskType] = useState('noop');
  const [delayMs, setDelayMs] = useState(1000);
  const [name, setName] = useState('');
  const [loading, setLoading] = useState(false);
  const [msg, setMsg] = useState(null);

  const handleSchedule = async (e) => {
    e.preventDefault();
    setLoading(true);
    setMsg(null);
    try {
      const res = await scheduleTask({
        task_type: taskType,
        name: name || undefined,
        delay_ms: parseInt(delayMs, 10),
      });
      setMsg({ type: 'success', text: `Scheduled ${res.data.task_id.slice(0, 8)}... in ${delayMs}ms` });
    } catch (err) {
      setMsg({ type: 'error', text: err.message });
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="space-y-6">
      <h2 className="text-2xl font-bold">Scheduler</h2>

      <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
        <h3 className="font-semibold mb-4">Schedule a Task</h3>
        <form onSubmit={handleSchedule} className="space-y-4 max-w-lg">
          <div className="grid grid-cols-2 gap-4">
            <div>
              <label className="block text-sm text-gray-500 mb-1">Task Type</label>
              <select value={taskType} onChange={(e) => setTaskType(e.target.value)}
                className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm">
                <option value="noop">No-Op</option>
                <option value="compute">Compute</option>
                <option value="sleep">Sleep</option>
              </select>
            </div>
            <div>
              <label className="block text-sm text-gray-500 mb-1">Delay (ms)</label>
              <input type="number" value={delayMs} onChange={(e) => setDelayMs(e.target.value)} min="1"
                className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm" />
            </div>
          </div>
          <div>
            <label className="block text-sm text-gray-500 mb-1">Name (optional)</label>
            <input type="text" value={name} onChange={(e) => setName(e.target.value)}
              className="w-full bg-gray-100 dark:bg-gray-800 border border-gray-300 dark:border-gray-700 rounded-lg px-3 py-2 text-sm"
              placeholder="Task description..." />
          </div>
          <button type="submit" disabled={loading}
            className="px-5 py-2 bg-blue-600 hover:bg-blue-700 disabled:bg-gray-400 text-white rounded-lg text-sm font-medium transition-colors">
            {loading ? 'Scheduling...' : 'Schedule Task'}
          </button>
          {msg && (
            <div className={`text-sm p-3 rounded-lg ${msg.type === 'success' ? 'bg-green-50 dark:bg-green-900/20 text-green-600 dark:text-green-400' : 'bg-red-50 dark:bg-red-900/20 text-red-600 dark:text-red-400'}`}>
              {msg.text}
            </div>
          )}
        </form>
      </div>

      <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
        <p className="text-sm text-gray-400">Scheduled tasks are submitted to the thread pool after their delay expires. Use the Scheduler API to manage recurring and delayed tasks.</p>
      </div>
    </div>
  );
}
