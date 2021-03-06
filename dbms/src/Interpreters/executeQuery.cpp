#include <DB/Common/ProfileEvents.h>
#include <DB/Common/formatReadable.h>

#include <DB/IO/ConcatReadBuffer.h>
#include <DB/IO/WriteBufferFromFile.h>

#include <DB/DataStreams/BlockIO.h>
#include <DB/DataStreams/copyData.h>
#include <DB/DataStreams/IProfilingBlockInputStream.h>
#include <DB/DataStreams/InputStreamFromASTInsertQuery.h>
#include <DB/DataStreams/CountingBlockOutputStream.h>

#include <DB/Parsers/ASTInsertQuery.h>
#include <DB/Parsers/ASTShowProcesslistQuery.h>
#include <DB/Parsers/ASTIdentifier.h>
#include <DB/Parsers/ASTLiteral.h>
#include <DB/Parsers/ParserQuery.h>
#include <DB/Parsers/parseQuery.h>

#include <DB/Interpreters/Quota.h>
#include <DB/Interpreters/InterpreterFactory.h>
#include <DB/Interpreters/ProcessList.h>
#include <DB/Interpreters/QueryLog.h>
#include <DB/Interpreters/executeQuery.h>


namespace ProfileEvents
{
	extern const Event Query;
}

namespace DB
{

namespace ErrorCodes
{
	extern const int LOGICAL_ERROR;
	extern const int QUERY_IS_TOO_LARGE;
	extern const int INTO_OUTFILE_NOT_ALLOWED;
}


static void checkLimits(const IAST & ast, const Limits & limits)
{
	if (limits.max_ast_depth)
		ast.checkDepth(limits.max_ast_depth);
	if (limits.max_ast_elements)
		ast.checkSize(limits.max_ast_elements);
}


static String joinLines(const String & query)
{
	String res = query;
	std::replace(res.begin(), res.end(), '\n', ' ');
	return res;
}


/// Log query into text log (not into system table).
static void logQuery(const String & query, const Context & context)
{
	LOG_DEBUG(&Logger::get("executeQuery"), "(from " << context.getClientInfo().current_address.toString() << ") " << joinLines(query));
}


/// Call this inside catch block.
static void setExceptionStackTrace(QueryLogElement & elem)
{
	try
	{
		throw;
	}
	catch (const Exception & e)
	{
		elem.stack_trace = e.getStackTrace().toString();
	}
	catch (...) {}
}


/// Log exception (with query info) into text log (not into system table).
static void logException(Context & context, QueryLogElement & elem)
{
	LOG_ERROR(&Logger::get("executeQuery"), elem.exception
		<< " (from " << context.getClientInfo().current_address.toString() << ")"
		<< " (in query: " << joinLines(elem.query) << ")"
		<< (!elem.stack_trace.empty() ? ", Stack trace:\n\n" + elem.stack_trace : ""));
}


static void onExceptionBeforeStart(const String & query, Context & context, time_t current_time)
{
	/// Exception before the query execution.
	context.getQuota().addError();

	bool log_queries = context.getSettingsRef().log_queries;

	/// Log the start of query execution into the table if necessary.
	if (log_queries)
	{
		QueryLogElement elem;

		elem.type = QueryLogElement::EXCEPTION_BEFORE_START;

		elem.event_time = current_time;
		elem.query_start_time = current_time;

		elem.query = query.substr(0, context.getSettingsRef().log_queries_cut_to_length);
		elem.exception = getCurrentExceptionMessage(false);

		elem.client_info = context.getClientInfo();

		setExceptionStackTrace(elem);
		logException(context, elem);

		context.getQueryLog().add(elem);
	}
}


static std::tuple<ASTPtr, BlockIO> executeQueryImpl(
	IParser::Pos begin,
	IParser::Pos end,
	Context & context,
	bool internal,
	QueryProcessingStage::Enum stage)
{
	ProfileEvents::increment(ProfileEvents::Query);
	time_t current_time = time(0);

	const Settings & settings = context.getSettingsRef();

	ParserQuery parser;
	ASTPtr ast;
	size_t query_size;
	size_t max_query_size = settings.max_query_size;

	try
	{
		ast = parseQuery(parser, begin, end, "");

		/// Copy query into string. It will be written to log and presented in processlist. If an INSERT query, string will not include data to insertion.
		query_size = ast->range.second - ast->range.first;

		if (max_query_size && query_size > max_query_size)
			throw Exception("Query is too large (" + toString(query_size) + ")."
				" max_query_size = " + toString(max_query_size), ErrorCodes::QUERY_IS_TOO_LARGE);
	}
	catch (...)
	{
		/// Anyway log query.
		if (!internal)
		{
			String query = String(begin, begin + std::min(end - begin, static_cast<ptrdiff_t>(max_query_size)));
			logQuery(query.substr(0, settings.log_queries_cut_to_length), context);
			onExceptionBeforeStart(query, context, current_time);
		}

		throw;
	}

	String query(begin, query_size);
	BlockIO res;

	try
	{
		if (!internal)
			logQuery(query.substr(0, settings.log_queries_cut_to_length), context);

		/// Check the limits.
		checkLimits(*ast, settings.limits);

		QuotaForIntervals & quota = context.getQuota();

		quota.addQuery();	/// NOTE Seems that when new time interval has come, first query is not accounted in number of queries.
		quota.checkExceeded(current_time);

		/// Put query to process list. But don't put SHOW PROCESSLIST query itself.
		ProcessList::EntryPtr process_list_entry;
		if (!internal && nullptr == typeid_cast<const ASTShowProcesslistQuery *>(&*ast))
		{
			process_list_entry = context.getProcessList().insert(
				query,
				ast.get(),
				context.getClientInfo(),
				settings);

			context.setProcessListElement(&process_list_entry->get());
		}

		auto interpreter = InterpreterFactory::get(ast, context, stage);
		res = interpreter->execute();

		/// Delayed initialization of query streams (required for KILL QUERY purposes)
		if (!internal && nullptr == typeid_cast<const ASTShowProcesslistQuery *>(&*ast))
			(*process_list_entry)->setQueryStreams(res);

		/// Hold element of process list till end of query execution.
		res.process_list_entry = process_list_entry;

		if (res.in)
		{
			if (auto stream = dynamic_cast<IProfilingBlockInputStream *>(res.in.get()))
			{
				stream->setProgressCallback(context.getProgressCallback());
				stream->setProcessListElement(context.getProcessListElement());
			}
		}

		if (res.out)
		{
			if (auto stream = dynamic_cast<CountingBlockOutputStream *>(res.out.get()))
			{
				stream->setProcessListElement(context.getProcessListElement());
			}
		}

		/// Everything related to query log.
		{
			QueryLogElement elem;

			elem.type = QueryLogElement::QUERY_START;

			elem.event_time = current_time;
			elem.query_start_time = current_time;

			elem.query = query.substr(0, settings.log_queries_cut_to_length);

			elem.client_info = context.getClientInfo();

			bool log_queries = settings.log_queries && !internal;

			/// Log into system table start of query execution, if need.
			if (log_queries)
				context.getQueryLog().add(elem);

			/// Also make possible for caller to log successful query finish and exception during execution.
			res.finish_callback = [elem, &context, log_queries] (IBlockInputStream * stream_in, IBlockOutputStream * stream_out) mutable
			{
				ProcessListElement * process_list_elem = context.getProcessListElement();

				if (!process_list_elem)
					return;

				double elapsed_seconds = process_list_elem->watch.elapsedSeconds();

				elem.type = QueryLogElement::QUERY_FINISH;

				elem.event_time = time(0);
				elem.query_duration_ms = elapsed_seconds * 1000;

				elem.read_rows = process_list_elem->progress_in.rows;
				elem.read_bytes = process_list_elem->progress_in.bytes;

				elem.written_rows = process_list_elem->progress_out.rows;
				elem.written_bytes = process_list_elem->progress_out.bytes;

				auto memory_usage = process_list_elem->memory_tracker.getPeak();
				elem.memory_usage = memory_usage > 0 ? memory_usage : 0;

				if (stream_in)
				{
					if (auto profiling_stream = dynamic_cast<const IProfilingBlockInputStream *>(stream_in))
					{
						const BlockStreamProfileInfo & info = profiling_stream->getProfileInfo();

						/// NOTE: INSERT SELECT query contains zero metrics
						elem.result_rows = info.rows;
						elem.result_bytes = info.bytes;
					}
				}
				else if (stream_out) /// will be used only for ordinary INSERT queries
				{
					if (auto counting_stream = dynamic_cast<const CountingBlockOutputStream *>(stream_out))
					{
						/// NOTE: Redundancy. The same values could be extracted from process_list_elem->progress_out.
						elem.result_rows = counting_stream->getProgress().rows;
						elem.result_bytes = counting_stream->getProgress().bytes;
					}
				}

				if (elem.read_rows != 0)
				{
					LOG_INFO(&Logger::get("executeQuery"), std::fixed << std::setprecision(3)
						<< "Read " << elem.read_rows << " rows, "
						<< formatReadableSizeWithBinarySuffix(elem.read_bytes) << " in " << elapsed_seconds << " sec., "
						<< static_cast<size_t>(elem.read_rows / elapsed_seconds) << " rows/sec., "
						<< formatReadableSizeWithBinarySuffix(elem.read_bytes / elapsed_seconds) << "/sec.");
				}

				if (log_queries)
					context.getQueryLog().add(elem);
			};

			res.exception_callback = [elem, &context, log_queries] () mutable
			{
				context.getQuota().addError();

				elem.type = QueryLogElement::EXCEPTION_WHILE_PROCESSING;

				elem.event_time = time(0);
				elem.query_duration_ms = 1000 * (elem.event_time - elem.query_start_time);
				elem.exception = getCurrentExceptionMessage(false);

				ProcessListElement * process_list_elem = context.getProcessListElement();

				if (process_list_elem)
				{
					double elapsed_seconds = process_list_elem->watch.elapsedSeconds();

					elem.query_duration_ms = elapsed_seconds * 1000;

					elem.read_rows = process_list_elem->progress_in.rows;
					elem.read_bytes = process_list_elem->progress_in.bytes;

					auto memory_usage = process_list_elem->memory_tracker.getPeak();
					elem.memory_usage = memory_usage > 0 ? memory_usage : 0;
				}

				setExceptionStackTrace(elem);
				logException(context, elem);

				if (log_queries)
					context.getQueryLog().add(elem);
			};

			if (!internal && res.in)
			{
				std::stringstream log_str;
				log_str << "Query pipeline:\n";
				res.in->dumpTree(log_str);
				LOG_DEBUG(&Logger::get("executeQuery"), log_str.str());
			}
		}
	}
	catch (...)
	{
		if (!internal)
			onExceptionBeforeStart(query, context, current_time);

		throw;
	}

	return std::make_tuple(ast, res);
}


BlockIO executeQuery(
	const String & query,
	Context & context,
	bool internal,
	QueryProcessingStage::Enum stage)
{
	BlockIO streams;
	std::tie(std::ignore, streams) = executeQueryImpl(query.data(), query.data() + query.size(), context, internal, stage);
	return streams;
}


void executeQuery(
	ReadBuffer & istr,
	WriteBuffer & ostr,
	bool allow_into_outfile,
	Context & context,
	std::function<void(const String &)> set_content_type)
{
	PODArray<char> parse_buf;
	const char * begin;
	const char * end;

	/// If 'istr' is empty now, fetch next data into buffer.
	if (istr.buffer().size() == 0)
		istr.next();

	size_t max_query_size = context.getSettingsRef().max_query_size;

	if (istr.buffer().end() - istr.position() >= static_cast<ssize_t>(max_query_size))
	{
		/// If remaining buffer space in 'istr' is enough to parse query up to 'max_query_size' bytes, then parse inplace.
		begin = istr.position();
		end = istr.buffer().end();
		istr.position() += end - begin;
	}
	else
	{
		/// If not - copy enough data into 'parse_buf'.
		parse_buf.resize(max_query_size);
		parse_buf.resize(istr.read(&parse_buf[0], max_query_size));
		begin = &parse_buf[0];
		end = begin + parse_buf.size();
	}

	ASTPtr ast;
	BlockIO streams;

	std::tie(ast, streams) = executeQueryImpl(begin, end, context, false, QueryProcessingStage::Complete);

	try
	{
		if (streams.out)
		{
			InputStreamFromASTInsertQuery in(ast, istr, streams, context);
			copyData(in, *streams.out);
		}

		if (streams.in)
		{
			const ASTQueryWithOutput * ast_query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get());

			WriteBuffer * out_buf = &ostr;
			std::experimental::optional<WriteBufferFromFile> out_file_buf;
			if (ast_query_with_output && ast_query_with_output->out_file)
			{
				if (!allow_into_outfile)
					throw Exception("INTO OUTFILE is not allowed", ErrorCodes::INTO_OUTFILE_NOT_ALLOWED);

				const auto & out_file = typeid_cast<const ASTLiteral &>(*ast_query_with_output->out_file).value.safeGet<std::string>();
				out_file_buf.emplace(out_file, DBMS_DEFAULT_BUFFER_SIZE, O_WRONLY | O_EXCL | O_CREAT);
				out_buf = &out_file_buf.value();
			}

			String format_name = ast_query_with_output && (ast_query_with_output->format != nullptr)
				? typeid_cast<const ASTIdentifier &>(*ast_query_with_output->format).name
				: context.getDefaultFormat();

			BlockOutputStreamPtr out = context.getOutputFormat(format_name, *out_buf, streams.in_sample);

			if (auto stream = dynamic_cast<IProfilingBlockInputStream *>(streams.in.get()))
			{
				/// Save previous progress callback if any. TODO Do it more conveniently.
				auto previous_progress_callback = context.getProgressCallback();

				/// NOTE Progress callback takes shared ownership of 'out'.
				stream->setProgressCallback([out, previous_progress_callback] (const Progress & progress)
				{
					if (previous_progress_callback)
						previous_progress_callback(progress);
					out->onProgress(progress);
				});
			}

			if (set_content_type)
				set_content_type(out->getContentType());

			copyData(*streams.in, *out);
		}
	}
	catch (...)
	{
		streams.onException();
		throw;
	}

	streams.onFinish();
}

}
