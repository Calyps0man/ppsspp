import * as net from 'node:net';
import * as http from 'node:http';
import * as fs from 'node:fs';
import * as worker_threads from 'node:worker_threads';
import * as os from 'node:os';

const port = 27313
const status_port = 27314;
const memory_usage_log_interval_ms =  1000 * 60 * 2;

const PDP_BLOCK_MAX = 10 * 1024;
const PTP_BLOCK_MAX = 50 * 1024;

// max chunk size per server tick per session, considering a 2ms refresh interval, 256 data size, broadcasting to 8 other players, and then half it considering this is by tick rate, which is already very generous
const MAX_TOTAL_CHUNK_SIZE = ((1000 / 2) * 256 * 8) / 2;

enum InitPacketType{
	AEMU_POSTOFFICE_INIT_PDP = 0,
	AEMU_POSTOFFICE_INIT_PTP_LISTEN = 1,
	AEMU_POSTOFFICE_INIT_PTP_CONNECT = 2,
	AEMU_POSTOFFICE_INIT_PTP_ACCEPT = 3,
}

enum SessionMode{
	SESSION_MODE_INIT = -1,
	SESSION_MODE_PDP = 0,
	SESSION_MODE_PTP_LISTEN = 1,
	SESSION_MODE_PTP_CONNECT = 2,
	SESSION_MODE_PTP_ACCEPT = 3,
}

enum PdpState{
	PDP_STATE_HEADER = 0,
	PDP_STATE_DATA = 1,
}

enum PtpState{
	PTP_STATE_WAITING = -1,
	PTP_STATE_HEADER = 0,
	PTP_STATE_DATA = 1,
}

enum ParentToWorkerMessageType{
	PARENT_MESSAGE_CREATE_SESSION = 0,
	PARENT_MESSAGE_REMOVE_SESSION = 1,
	PARENT_MESSAGE_HANDLE_CHUNK = 2,
	PARENT_MESSAGE_ADD_SESSION_IP = 3,
	PARENT_MESSAGE_REMOVE_SESSION_IP = 4,
	PARENT_MESSAGE_SYNC_ADHOCCTL_DATA = 5,
	PARENT_MESSAGE_UPDATE_CONFIG = 6,
}

enum WorkerToParentMessageType{
	WORKER_MESSAGE_REMOVE_SESSION = 0,
	WORKER_MESSAGE_SEND_DATA = 1,
}

enum SendType{
	SEND_TYPE_PDP = 0,
	SEND_TYPE_PTP = 1,
}

interface SessionFromParent{
	src_addr:Uint8Array,
	sport:number,
	dst_addr:Uint8Array,
	dport:number,
	src_addr_str:string,
	dst_addr_str:string,
	state:SessionMode,
	session_name:string,
	pdp_data:Uint8Array,
	ptp_data:Uint8Array,
	peer_session_name:string,
	pdp_state:PdpState,
	ptp_state:PtpState,
	sock_addr_str:string,
}

interface WorkerSession{
	src_addr:Buffer,
	sport:number,
	dst_addr:Buffer,
	dport:number,
	src_addr_str:string,
	dst_addr_str:string,
	state:SessionMode,
	session_name:string,
	pdp_data:Buffer,
	ptp_data:Buffer,
	peer_session_name:string,
	pdp_state:PdpState,
	ptp_state:PtpState,
	sock_addr_str:string,
	target_session_name:string,
	target_mac:string,
	pdp_data_size:number,
	ptp_data_size:number,
}

interface Worker{
	id:number,
	num_sessions:number,
	worker:worker_threads.Worker,
}

interface Session{
	src_addr:Buffer,
	sport:number,
	dst_addr:Buffer,
	dport:number,
	src_addr_str:string,
	dst_addr_str:string,
	state:SessionMode,
	session_name:string,
	pdp_data:Buffer,
	ptp_data:Buffer,
	peer_session_name:string,
	pdp_state:PdpState,
	ptp_state:PtpState,
	sock_addr_str:string,
	socket:net.Socket,
	worker?:Worker,
	chunks:Buffer[],
	peer_session?:Session,
	init_data:Buffer,
	outstanding_data:Buffer,
	outstanding_data_created:boolean,
	ip:string,
	ptp_wait_timeout:any,
	init_timeout:any,
	ptp_connect_retries:number,
}

interface SessionMap{
	[index:string]:Session,
}

interface WorkerSessionMap{
	[index:string]:WorkerSession,
}

interface AddrSessionsMap{
	[index:string]:SessionMap,
}

interface SessionIpMap{
	[index:string]:string,
}

interface SendListItem{
	from_session_name:string,
	from_mac:string,
	to_session_name:string,
	to_mac:string,
	data:Buffer,
	send_type:SendType,
}

interface SendListItemToParent{
	to_session_name:string,
	to_mac:string,
	data:Buffer[],
}

interface SendListItemFromWorker{
	to_session_name:string,
	to_mac:string,
	data:Uint8Array[],
}

process.on('SIGTERM', () => {
   process.exit(0);
});

process.on('SIGINT', () => {
   process.exit(0);
});

let sessions:SessionMap = {};
let sessions_by_mac:AddrSessionsMap = {};
let sessions_by_ip:AddrSessionsMap = {};

let session_ip_lookup:SessionIpMap = {};
let worker_sessions:WorkerSessionMap = {};

interface AdhocctlPlayer{
	mac_addr:string,
	ip_addr:string,
}

interface AdhocctlGroup{
	[index:string]:AdhocctlPlayer,
}

interface AdhocctlGame{
	groups:AdhocctlGroup[],
}

interface AdhocctlData{
	games:AdhocctlGame[],
}

let adhocctl_data:AdhocctlData = {games:[]};
let adhocctl_groups_by_mac:{[index:string]:AdhocctlGroup} = {};
let adhocctl_players_by_mac:{[index:string]:AdhocctlPlayer} = {};

let workers:Worker[] = [];
let send_list:SendListItem[] = [];

interface Config{
	[index:string]:any
}

let config:Config = {
	connection_strict_mode:false,
	forwarding_strict_mode:false,
	max_per_second_data_rate_byte:0,
	max_tx_op_rate:0,
	accounting_interval_ms:30000,
	max_write_buffer_byte:512000,
	max_connections:5000,
	num_worker_threads:1,
	tick_rate_hz:90,
	max_ips:0,
};

function log(...args:any){
	console.log(new Date().toISOString(), ...args);
};

function load_config(){
	let config_path = "./config.json";
	if (process.env.AEMU_POSTOFFICE_CONFIG_PATH != undefined){
		config_path = process.env.AEMU_POSTOFFICE_CONFIG_PATH;
	}
	if (worker_threads.isMainThread){
		log(`loading config from ${config_path}`);
	}

	try{
		const file_str = fs.readFileSync(config_path, {encoding:"utf8"});

		let parsed_data = JSON.parse(file_str);
		for(const [key, value] of Object.entries(parsed_data)){
			config[key] = value;
		}
	}catch(e){
		log(`warning: failed parsing config.json, ${e}`);
	}

	if (worker_threads.isMainThread){
		log(`runtime config:\n${JSON.stringify(config, null, 4)}`);

		if (config.num_worker_threads <= 0){
			log(`number of worker threads cannot be less than one (${config.num_worker_threads}), please change your config`);
			process.exit(1);
		}

		if (config.accounting_interval_ms <= 0){
			log(`warning: accounting is disabled, statistics logging is disabled, "max_per_second_data_rate_byte" and "max_tx_op_rate" are now not enforced`);
		}

		if (config.tick_rate_hz < 30 || config.tick_rate_hz > 1000){
			log(`bad tickrate ${config.tick_rate_hz} configured (accepts 30 - 1000), please change your config`);
			process.exit(1);
		}
	}
}

load_config();

function get_mac_str(mac:Buffer | Uint8Array){
	let ret = ""
	for (let i = 0;i < 6;i++){
		if (i != 0){
			ret = ret + ":";
		}
		ret = ret + mac.subarray(i, i + 1).toString("hex");
	}
	return ret;
}

function get_sock_addr_str(sock:net.Socket){
	return `${sock.remoteAddress}:${sock.remotePort}`
}

interface StatisticsEntry{
	ptp_connects:number,
	ptp_listen_connects:number,
	ptp_tx:number,
	ptp_tx_ops:number,
	ptp_rx:number,
	ptp_rx_ops:number,
	pdp_connects:number,
	pdp_tx:number,
	pdp_tx_ops:number,
	pdp_rx:number,
	pdp_rx_ops:number,
}

interface Statistics {
	[index:string]:StatisticsEntry;
}

// simple tracking per interval statistics for now, a better picture requires a database
let statistics:Statistics = {};

function update_statistics(update:Statistics, base:Statistics = statistics):undefined{
	for (const [ip, value] of Object.entries(update)){
		let base_value = base[ip];
		if (base_value == undefined){
			base[ip] = value;
			continue;
		}
		base_value.ptp_connects += value.ptp_connects;
		base_value.ptp_listen_connects += value.ptp_listen_connects;
		base_value.ptp_tx += value.ptp_tx;
		base_value.ptp_tx_ops += value.ptp_tx_ops;
		base_value.ptp_rx += value.ptp_rx;
		base_value.ptp_rx_ops += value.ptp_rx_ops;
		base_value.pdp_connects += value.pdp_connects;
		base_value.pdp_tx += value.pdp_tx;
		base_value.pdp_tx_ops += value.pdp_tx_ops;
		base_value.pdp_rx += value.pdp_rx;
		base_value.pdp_rx_ops += value.pdp_rx_ops;
	}
}

function get_statistics_obj(ip:string, container:Statistics = statistics):StatisticsEntry{
	let existing_obj = container[ip];
	if (existing_obj != undefined){
		return existing_obj;
	}
	let new_obj:StatisticsEntry = {
		ptp_connects:0,
		ptp_listen_connects:0,
		ptp_tx:0,
		ptp_tx_ops:0,
		ptp_rx:0,
		ptp_rx_ops:0,
		pdp_connects:0,
		pdp_tx:0,
		pdp_tx_ops:0,
		pdp_rx:0,
		pdp_rx_ops:0
	};
	container[ip] = new_obj;
	return new_obj;
}

function track_connect(ip:string, is_ptp:boolean, is_listen:boolean, container:Statistics = statistics):void{
	let statistics_obj = get_statistics_obj(ip, container);
	if (is_ptp){
		if (is_listen){
			statistics_obj.ptp_listen_connects++;
		}else{
			statistics_obj.ptp_connects++;
		}
	}else{
		statistics_obj.pdp_connects++;
	}
}

function track_bandwidth(ip:string, is_ptp:boolean, is_tx:boolean, size:number, container:Statistics = statistics):void{
	let statistics_obj = get_statistics_obj(ip, container);
	if (is_ptp){
		if (is_tx){
			statistics_obj.ptp_tx += size;
			statistics_obj.ptp_tx_ops++;
		}else{
			statistics_obj.ptp_rx += size;
			statistics_obj.ptp_rx_ops++;
		}
	}else{
		if (is_tx){
			statistics_obj.pdp_tx += size;
			statistics_obj.pdp_tx_ops++;
		}else{
			statistics_obj.pdp_rx += size;
			statistics_obj.pdp_rx_ops++;
		}
	}
}

function output_memory_usage(){
	console.log(`--- memory usage ${new Date()} ---`);
	console.log(JSON.stringify(process.memoryUsage(), null, 4));
}

function set_interval(func:() => void, interval_ms_num:number){
	let interval_ms:bigint = BigInt(Math.floor(interval_ms_num));

	let wrapper = () => {
		let begin_ns:bigint = process.hrtime.bigint();
		func();
		let duration_ms:bigint = (process.hrtime.bigint() - begin_ns) / BigInt(1000000);
		let wait_ms:bigint = interval_ms - duration_ms;
		if (wait_ms <= 0){
			wait_ms = BigInt(0);
		}
		setTimeout(wrapper, Number(wait_ms));
	};
	wrapper();
}

function run_per_tick(func:() => void){
	let wrapper = () => {
		let begin_ns:bigint = process.hrtime.bigint();
		func();
		let duration_ms:bigint = (process.hrtime.bigint() - begin_ns) / BigInt(1000000);
		let wait_ms:bigint = BigInt(Math.floor(1000 / config.tick_rate_hz)) - duration_ms;
		if (wait_ms <= 0){
			wait_ms = BigInt(0);
		}
		setTimeout(wrapper, Number(wait_ms));
	};
	wrapper();
}

if (worker_threads.isMainThread){
	set_interval(output_memory_usage, memory_usage_log_interval_ms);
}

// don't pull statistics into a scope with arrow function
function output_statistics(){
	let entries = Object.entries(statistics);
	if (entries.length == 0){
		return;
	}

	console.log(`--- usage statistics ${new Date()} of the last ${config.accounting_interval_ms / 1000 / 60} minutes ---`);

	let interval_s = config.accounting_interval_ms / 1000;

	for (let entry of entries){
		let ip = entry[0];
		let obj = entry[1];
		console.log(`${ip}:`);

		console.log(`  pdp connects ${obj.pdp_connects} avg ${obj.pdp_connects / interval_s}/s`);

		console.log(`  pdp tx ops ${obj.pdp_tx_ops} avg ${obj.pdp_tx_ops / interval_s}/s`);
		console.log(`  pdp tx ${obj.pdp_tx} bytes avg ${obj.pdp_tx / interval_s} bytes/s`);
		console.log(`  pdp rx ops ${obj.pdp_rx_ops} avg ${obj.pdp_rx_ops / interval_s}/s`);
		console.log(`  pdp rx ${obj.pdp_rx} bytes avg ${obj.pdp_rx / interval_s} bytes/s`);

		console.log(`  ptp connects ${obj.ptp_connects} avg ${obj.ptp_connects / interval_s}/s`);
		console.log(`  ptp listen connects ${obj.ptp_listen_connects} avg ${obj.ptp_listen_connects / interval_s}/s`);

		console.log(`  ptp tx ops ${obj.ptp_tx_ops} avg ${obj.ptp_tx_ops / interval_s}/s`);
		console.log(`  ptp tx ${obj.ptp_tx} bytes avg ${obj.ptp_tx / interval_s} bytes/s`);
		console.log(`  ptp rx ops ${obj.ptp_rx_ops} avg ${obj.ptp_rx_ops / interval_s}/s`);
		console.log(`  ptp rx ${obj.ptp_rx} bytes avg ${obj.ptp_rx / interval_s} bytes/s`);

		let total_connects = obj.pdp_connects + obj.ptp_connects + obj.ptp_listen_connects;
		let total_tx_ops = obj.pdp_tx_ops + obj.ptp_tx_ops;
		let total_rx_ops = obj.pdp_rx_ops + obj.ptp_rx_ops;
		let total_ops = total_tx_ops + total_rx_ops;
		let total_tx = obj.pdp_tx + obj.ptp_tx;
		let total_rx = obj.pdp_rx + obj.ptp_rx;
		let total_data = total_tx + total_rx;

		console.log(`  total connects: ${total_connects} avg ${total_connects / interval_s}/s`);
		console.log(`  total tx ops: ${total_tx_ops} avg ${total_tx_ops / interval_s}/s`);
		console.log(`  total rx ops: ${total_rx_ops} avg ${total_rx_ops / interval_s}/s`);
		console.log(`  total ops: ${total_ops} avg ${total_ops / interval_s}/s`);
		console.log(`  total tx: ${total_tx} avg ${total_tx / interval_s} bytes/s`);
		console.log(`  total rx: ${total_rx} avg ${total_rx / interval_s} bytes/s`);
		console.log(`  total data: ${total_data} avg ${total_data / interval_s} bytes/s`);
	}
}

function remove_session_ip_in_workers(session_name:string){
	const message = {
		type:ParentToWorkerMessageType.PARENT_MESSAGE_REMOVE_SESSION_IP,
		session_name:session_name,
	};

	for(let worker of workers){
		worker.worker.postMessage(message);
	}
}

function close_one_session(ctx:Session){
	// in case we get into the edge case of new session added before delayed removal, we want to keep track of this
	let session_deleted:boolean = false;

	log(`closing ${ctx.session_name}`);
	ctx.socket.destroy();
	if (ctx == sessions[ctx.session_name]){
		session_deleted = true;
		delete sessions[ctx.session_name];
	}
	let sessions_of_this_mac = sessions_by_mac[ctx.src_addr_str];
	if (sessions_of_this_mac != undefined){
		if (ctx == sessions_of_this_mac[ctx.session_name]){
			delete sessions_of_this_mac[ctx.session_name];
		}
		if (Object.keys(sessions_of_this_mac).length == 0){
			delete sessions_by_mac[ctx.src_addr_str];
		}
	}

	let sessions_of_this_ip = sessions_by_ip[ctx.ip];
	if (sessions_of_this_ip != undefined){
		if (ctx == sessions_of_this_ip[ctx.session_name]){
			delete sessions_of_this_ip[ctx.session_name];
		}
		if (Object.keys(sessions_of_this_ip).length == 0){
			delete sessions_by_ip[ctx.ip];
		}
	}

	if (session_deleted){
		// if we have not replaced the session already, we push a delete message to the workers
		if (ctx.worker != undefined){
			ctx.worker.worker.postMessage({
				type:ParentToWorkerMessageType.PARENT_MESSAGE_REMOVE_SESSION,
				session_name:ctx.session_name,
			});
			ctx.worker.num_sessions--;
		}

		remove_session_ip_in_workers(ctx.session_name);
	}
}

function close_session(ctx:Session){
	close_one_session(ctx);

	if (ctx.peer_session != undefined){
		close_one_session(ctx.peer_session);
	}
}

function check_bandwidth_limit(){
	if (config.max_per_second_data_rate_byte == 0 && config.max_tx_op_rate == 0){
		return;
	}

	for (const [ip, usage] of Object.entries(statistics)){
		const interval_s = config.accounting_interval_ms / 1000;
		const total_tx = usage.pdp_tx + usage.ptp_tx;
		const total_tx_ops = usage.pdp_tx_ops + usage.ptp_tx_ops;
		const tx_per_second = total_tx / interval_s;
		const tx_ops_per_second = total_tx_ops / interval_s;

		if (config.max_per_second_data_rate_byte != 0 && tx_per_second > config.max_per_second_data_rate_byte){
			log(`ip address ${ip} is sending more than ${config.max_per_second_data_rate_byte} bytes per second (${tx_per_second}), purging sessions`);
			const sessions_of_this_ip = sessions_by_ip[ip];
			if (sessions_of_this_ip != undefined){
				for (const session of Object.values(sessions_of_this_ip)){
					close_session(session);
				}
			}
		}

		if (config.max_tx_op_rate != 0 && tx_ops_per_second > config.max_tx_op_rate){
			log(`ip address ${ip} is doing more than ${config.max_tx_op_rate} tx ops per second (${tx_ops_per_second}), purging sessions`);
			const sessions_of_this_ip = sessions_by_ip[ip];
			if (sessions_of_this_ip != undefined){
				for (const session of Object.values(sessions_of_this_ip)){
					close_session(session);
				}
			}
		}
	}
}

function process_statistics(){
	if (config.accounting_interval_ms >= 0){
		output_statistics();
		check_bandwidth_limit();
		statistics = {};
	}
	let wait_ms = config.accounting_interval_ms;
	if (wait_ms <= 0){
		wait_ms = 1000;
	}
	setTimeout(process_statistics, wait_ms);
}

if (worker_threads.isMainThread){
	process_statistics();
}

function get_target_session_name(mode:SessionMode, my_mac:string, mac:string, sport:number, dport:number){
	switch(mode){
		case SessionMode.SESSION_MODE_PDP:
			return `PDP ${mac} ${dport}`;
		case SessionMode.SESSION_MODE_PTP_LISTEN:
			return `PTP_LISTEN ${mac} ${dport}`;
		case SessionMode.SESSION_MODE_PTP_CONNECT:
			return `PTP_CONNECT ${mac} ${dport} ${my_mac} ${sport}`;
		default:
			log(`bad mode ${mode}, debug this`);
			process.exit(1);
	}
}

function find_target_session(mode:SessionMode, my_mac:string, mac:string, sport:number, dport:number){
	if (config.forwarding_strict_mode){
		const adhocctl_group = adhocctl_groups_by_mac[my_mac];
		if (adhocctl_group == undefined){
			return undefined;
		}
		let found = adhocctl_group[mac] != undefined;
		if (!found){
			return undefined;
		}
	}

	const target_session_name = get_target_session_name(mode, my_mac, mac, sport, dport);

	const sessions_of_this_mac = sessions_by_mac[mac];
	if (sessions_of_this_mac == undefined){
		return undefined;
	}
	return sessions_of_this_mac[target_session_name];
}

function send_data_to_parent(){
	if (send_list.length == 0){
		return;
	}
	let statistics_update:Statistics = {};
	let organized_send_list:{[index:string]:SendListItemToParent} = {};
	//let transfer_list = [];
	for (const send of send_list){
		const to_ip = session_ip_lookup[send.to_session_name];
		const from_ip = session_ip_lookup[send.from_session_name];

		// if we can ensure that the session ip list is always complete when chunks are sent, we can actually drop packets with missing to ip

		if (config.forwarding_strict_mode && send.send_type == SendType.SEND_TYPE_PDP){
			const from_group = adhocctl_groups_by_mac[send.from_mac];
			const to_group = adhocctl_groups_by_mac[send.to_mac];
			if (from_group == undefined || to_group == undefined){
				continue;
			}
			if (from_group != to_group){
				continue;
			}
		}

		// merge the sends per session name
		let send_item_of_this_dst = organized_send_list[send.to_session_name];
		if (send_item_of_this_dst == undefined){
			send_item_of_this_dst = {
				to_session_name:send.to_session_name,
				to_mac:send.to_mac,
				data:[send.data],
			};
			organized_send_list[send.to_session_name] = send_item_of_this_dst;
		}else{
			send_item_of_this_dst.data.push(send.data);
		}
		//transfer_list.push(send.data.buffer);

		// evaluate statistics
		if (config.accounting_interval_ms <= 0){
			continue;
		}

		if (to_ip == undefined || from_ip == undefined){
			// send has to be done, incase we somehow fell behind the session ip update message
			// we can let this slide however if it's just for stats
			continue;
		}

		switch(send.send_type){
			case SendType.SEND_TYPE_PDP:
				track_bandwidth(to_ip, false, false, send.data.length - 14, statistics_update);
				track_bandwidth(from_ip, false, true, send.data.length - 14, statistics_update);
				break;
			case SendType.SEND_TYPE_PTP:
				track_bandwidth(to_ip, true, false, send.data.length - 4, statistics_update);
				track_bandwidth(from_ip, true, true, send.data.length - 4, statistics_update);
				break;
			default:
				log(`bad send type ${send.send_type} while organizing statistics update, debug this`);
				process.exit(1);
		}
	}

	if (worker_threads.parentPort != undefined){
		worker_threads.parentPort.postMessage({
			type:WorkerToParentMessageType.WORKER_MESSAGE_SEND_DATA,
			send_list:Object.values(organized_send_list),
			statistics_update:statistics_update,
		});
	}
	send_list = [];
}

function send_remove_session_message_to_parent(session_name:string){
	if (worker_threads.parentPort != undefined){
		worker_threads.parentPort.postMessage({
			type:WorkerToParentMessageType.WORKER_MESSAGE_REMOVE_SESSION,
			session_name:session_name,
		});
	}
}

function pdp_tick(ctx:WorkerSession){
	let no_data = false;
	while(!no_data){
		switch(ctx.pdp_state){
			case PdpState.PDP_STATE_HEADER:{
				if (ctx.pdp_data.length >= 14){
					let cur_data = ctx.pdp_data.subarray(0, 14);
					ctx.pdp_data = ctx.pdp_data.subarray(14);

					let addr = cur_data.subarray(0, 8);
					let port = cur_data.subarray(8, 10).readUInt16LE();
					let size = cur_data.subarray(10, 14).readUInt32LE();

					if (size > PDP_BLOCK_MAX * 2){
						log(`${ctx.session_name} ${ctx.sock_addr_str} is sending way too big data with size ${size}, ending session`);
						send_remove_session_message_to_parent(ctx.session_name);
						return;
					}

					ctx.target_mac = get_mac_str(addr);
					ctx.target_session_name = get_target_session_name(SessionMode.SESSION_MODE_PDP, ctx.src_addr_str, ctx.target_mac, 0, port);
					ctx.pdp_data_size = size;

					ctx.pdp_state = PdpState.PDP_STATE_DATA;
				}else{
					no_data = true;
				}
				break;
			}
			case PdpState.PDP_STATE_DATA:{
				if (ctx.pdp_data.length >= ctx.pdp_data_size){
					let cur_data = ctx.pdp_data.subarray(0, ctx.pdp_data_size);
					ctx.pdp_data = ctx.pdp_data.subarray(ctx.pdp_data_size);

					let packet = Buffer.allocUnsafe(14 + ctx.pdp_data_size);
					ctx.src_addr.copy(packet);
					packet.writeUInt16LE(ctx.sport, 8);
					packet.writeUInt32LE(cur_data.length, 10);
					cur_data.copy(packet, 14);

					send_list.push({
						from_session_name:ctx.session_name,
						from_mac:ctx.src_addr_str,
						to_session_name:ctx.target_session_name,
						to_mac:ctx.target_mac,
						data:packet,
						send_type:SendType.SEND_TYPE_PDP,
					});
					ctx.pdp_state = PdpState.PDP_STATE_HEADER;
				}else{
					no_data = true;
				}
				break;
			}
			default:
				log(`bad state ${ctx.pdp_state} in pdp tick, debug this`);
				process.exit(1);
		}
	}
}

function ptp_tick(ctx:WorkerSession){
	let no_data = false;
	while(!no_data){
		switch(ctx.ptp_state){
			case PtpState.PTP_STATE_HEADER:{
				if (ctx.ptp_data.length >= 4){
					let cur_data = ctx.ptp_data.subarray(0, 4);
					ctx.ptp_data = ctx.ptp_data.subarray(4);

					let size = cur_data.readUInt32LE();
					if (size > PTP_BLOCK_MAX * 2){
						log(`${ctx.session_name} ${ctx.sock_addr_str} is sending way too big data with size ${size}, ending session`);
						send_remove_session_message_to_parent(ctx.session_name);
						return;
					}

					ctx.ptp_data_size = size;
					ctx.ptp_state = PtpState.PTP_STATE_DATA;
				}else{
					no_data = true;
				}
				break;
			}
			case PtpState.PTP_STATE_DATA:{
				if (ctx.ptp_data.length >= ctx.ptp_data_size){
					let cur_data = ctx.ptp_data.subarray(0, ctx.ptp_data_size);
					ctx.ptp_data = ctx.ptp_data.subarray(ctx.ptp_data_size);

					let packet = Buffer.allocUnsafe(4 + ctx.ptp_data_size);
					packet.writeUInt32LE(ctx.ptp_data_size);
					cur_data.copy(packet, 4);

					send_list.push({
						from_session_name:ctx.session_name,
						from_mac:ctx.src_addr_str,
						to_session_name:ctx.peer_session_name,
						to_mac:ctx.dst_addr_str,
						data:packet,
						send_type:SendType.SEND_TYPE_PTP,
					});
					ctx.ptp_state = PtpState.PTP_STATE_HEADER;
				}else{
					no_data = true;
				}
				break;
			}
			default:
				log(`bad state ${ctx.ptp_state} in ptp tick, debug this`);
				process.exit(1);
		}
	}
}

function remove_existing_and_insert_session(ctx:Session, name:string){
	const existing_session = sessions[name];
	if (existing_session != undefined){
		log(`dropping session ${existing_session.session_name} ${existing_session.sock_addr_str} for new session`);
		switch(existing_session.state){
			case SessionMode.SESSION_MODE_PDP:
			case SessionMode.SESSION_MODE_PTP_LISTEN:{
				close_session(existing_session);
				break;
			}
			case SessionMode.SESSION_MODE_PTP_CONNECT:
			case SessionMode.SESSION_MODE_PTP_ACCEPT:{
				close_session(existing_session);
				break;
			}
			default:
				log(`bad state ${existing_session.state} in session replacement, debug this`);
				process.exit(1);
		}
	}

	sessions[name] = ctx;
	let sessions_of_this_mac = sessions_by_mac[ctx.src_addr_str];
	if (sessions_of_this_mac == undefined){
		sessions_of_this_mac = {};
		sessions_by_mac[ctx.src_addr_str] = sessions_of_this_mac;
	}
	sessions_of_this_mac[name] = ctx;

	let sessions_of_this_ip = sessions_by_ip[ctx.ip];
	if (sessions_of_this_ip == undefined){
		sessions_of_this_ip = {};
		sessions_by_ip[ctx.ip] = sessions_of_this_ip;
	}
	sessions_of_this_ip[name] = ctx;
}

function strict_mode_verify_ip_addr(mac_addr:string, ip_addr:string){
	if (!config.connection_strict_mode){
		return true;
	}
	const player = adhocctl_players_by_mac[mac_addr];
	if (player == undefined){
		log(`strict mode: player with mac address ${mac_addr} not found, rejecting`);
		return false;
	}
	if (ip_addr != player.ip_addr){
		log(`strict mode: player with mac address ${mac_addr} should have ip addres ${player.ip_addr} instead of ${ip_addr}, rejecting`);
		return false;
	}
	return true;
}

function close_session_by_name(name:string){
	let session = sessions[name];
	if (session != undefined){
		close_session(session);
	}
}

function send_data_to_sessions(send_list:SendListItemFromWorker[]){
	for (const send of send_list){
		const sessions_of_to_mac = sessions_by_mac[send.to_mac];
		if (sessions_of_to_mac == undefined){
			continue;
		}
		let to_session = sessions_of_to_mac[send.to_session_name];
		if (to_session == undefined){
			continue;
		}

		to_session.socket.write(Buffer.concat(send.data));
		const max_buffer_size = config.max_write_buffer_byte;
		if (max_buffer_size != 0 && to_session.socket.writableLength >= max_buffer_size){
			log(`killing session ${to_session.session_name} as write buffer has reached ${to_session.socket.writableLength} bytes, max ${max_buffer_size} bytes`);
			close_session(to_session);
		}
	}
}

function handle_worker_message(m:any){
	switch(m.type){
		case WorkerToParentMessageType.WORKER_MESSAGE_REMOVE_SESSION:
			close_session_by_name(m.session_name);
			break;
		case WorkerToParentMessageType.WORKER_MESSAGE_SEND_DATA:
			send_data_to_sessions(m.send_list);
			if (config.accounting_interval_ms > 0){
				update_statistics(m.statistics_update);
			}
			break;
		default:
			log(`unknown worker message type ${m.type}, debug this`);
			process.exit(1);
	}
}

interface ChunkListEntryToWorker{
	session_name:string,
	chunks:Buffer[]
}

interface ChunkListEntryFromParent{
	session_name:string,
	chunks:Uint8Array[]
}

function handle_chunks_from_parent(chunk_list:ChunkListEntryFromParent[]){
	for (const chunk of chunk_list){
		let target_session = worker_sessions[chunk.session_name];
		if (target_session == undefined){
			log(`warning: worker/coordinator desync during chunk processing from parent, probably needs debugging`);
			continue;
		}
		switch(target_session.state){
			case SessionMode.SESSION_MODE_PDP:{
				chunk.chunks.unshift(target_session.pdp_data);
				target_session.pdp_data = Buffer.concat(chunk.chunks);
				if (target_session.pdp_data.length >= MAX_TOTAL_CHUNK_SIZE){
					log(`${target_session.session_name} is sending too much data, evicting`);
					send_remove_session_message_to_parent(target_session.session_name);
					break;
				}

				pdp_tick(target_session);
				break;
			}
			case SessionMode.SESSION_MODE_PTP_CONNECT:
			case SessionMode.SESSION_MODE_PTP_ACCEPT:{
				chunk.chunks.unshift(target_session.ptp_data);
				target_session.ptp_data = Buffer.concat(chunk.chunks);
				if (target_session.ptp_data.length >= MAX_TOTAL_CHUNK_SIZE){
					log(`${target_session.session_name} is sending too much data, evicting`);
					send_remove_session_message_to_parent(target_session.session_name);
					break;
				}

				ptp_tick(target_session);
				break;
			}
			default:
				log(`bad session state ${target_session.state} while handling chunk from parent, debug this`);
				process.exit(1);
		}
	}
}

function session_first_tick(session:WorkerSession){
	session.src_addr = Buffer.from(session.src_addr);
	switch(session.state){
		case SessionMode.SESSION_MODE_PDP:
			pdp_tick(session);
			break;
		case SessionMode.SESSION_MODE_PTP_CONNECT:
		case SessionMode.SESSION_MODE_PTP_ACCEPT:
			ptp_tick(session);
			break;
		default:
			log(`bad session state ${session.state} during first tick in worker, please debug this`);
			process.exit(1);
	}
}

function create_session_from_parent(session:SessionFromParent){
	let worker_session:WorkerSession = {
		src_addr:Buffer.from(session.src_addr),
		sport:session.sport,
		dst_addr:Buffer.from(session.dst_addr),
		dport:session.dport,
		src_addr_str:session.src_addr_str,
		dst_addr_str:session.dst_addr_str,
		state:session.state,
		session_name:session.session_name,
		pdp_data:Buffer.from(session.pdp_data),
		ptp_data:Buffer.from(session.ptp_data),
		peer_session_name:session.peer_session_name,
		pdp_state:session.pdp_state,
		ptp_state:session.ptp_state,
		sock_addr_str:session.sock_addr_str,
		target_session_name:"",
		target_mac:"",
		pdp_data_size:0,
		ptp_data_size:0,
	};
	worker_sessions[session.session_name] = worker_session;
	session_first_tick(worker_session);
}

function update_session_ip_lookup(session_name:string, ip:string){
	session_ip_lookup[session_name] = ip;
}

function remove_worker_session(session_name:string){
	delete worker_sessions[session_name];
}

function update_adhocctl_data_from_parent(new_data:{[index:string]:AdhocctlGroup}){
	adhocctl_groups_by_mac = new_data;
}

function delete_from_session_ip_lookup(session_name:string){
	delete session_ip_lookup[session_name];
}

function sync_config_from_parent(parent_config:Config){
	log(`syncing config from parent`);
	config = parent_config;
}

function handle_parent_message(m:any){
	switch(m.type){
		case ParentToWorkerMessageType.PARENT_MESSAGE_CREATE_SESSION:
			create_session_from_parent(m.session);
			break;
		case ParentToWorkerMessageType.PARENT_MESSAGE_REMOVE_SESSION:
			remove_worker_session(m.session_name);
			break;
		case ParentToWorkerMessageType.PARENT_MESSAGE_HANDLE_CHUNK:
			handle_chunks_from_parent(m.chunk_list);
			break;
		case ParentToWorkerMessageType.PARENT_MESSAGE_ADD_SESSION_IP:
			update_session_ip_lookup(m.session_name, m.ip);
			break;
		case ParentToWorkerMessageType.PARENT_MESSAGE_SYNC_ADHOCCTL_DATA:
			update_adhocctl_data_from_parent(m.adhocctl_groups_by_mac);
			break;
		case ParentToWorkerMessageType.PARENT_MESSAGE_REMOVE_SESSION_IP:
			delete_from_session_ip_lookup(m.session_name);
			break;
		case ParentToWorkerMessageType.PARENT_MESSAGE_UPDATE_CONFIG:
			sync_config_from_parent(m.config);
			break;
		default:
			log(`unknown parent message type ${m.type}, debug this`);
			process.exit(1);
	}
}

function add_session_to_worker(session:Session){
	let least_sessions_worker:Worker = workers[0];
	for (let worker of workers){
		if (least_sessions_worker.num_sessions > worker.num_sessions){
			least_sessions_worker = worker;
		}
	}
	let worker_session:WorkerSession = {
		src_addr:session.src_addr,
		sport:session.sport,
		dst_addr:session.dst_addr,
		dport:session.dport,
		src_addr_str:session.src_addr_str,
		dst_addr_str:session.dst_addr_str,
		state:session.state,
		session_name:session.session_name,
		pdp_data:session.pdp_data,
		ptp_data:session.ptp_data,
		peer_session_name:session.peer_session_name,
		pdp_state:session.pdp_state,
		ptp_state:session.ptp_state,
		sock_addr_str:session.sock_addr_str,
		target_session_name:"",
		target_mac:"",
		pdp_data_size:0,
		ptp_data_size:0,
	};
	least_sessions_worker.worker.postMessage({
		type:ParentToWorkerMessageType.PARENT_MESSAGE_CREATE_SESSION,
		session:worker_session,
	});
	least_sessions_worker.num_sessions++;
	session.pdp_data = Buffer.allocUnsafe(0);
	session.ptp_data = Buffer.allocUnsafe(0);
	session.worker = least_sessions_worker;
}

function add_session_ip_to_workers(session_name:string, ip:string){
	const message = {
		type:ParentToWorkerMessageType.PARENT_MESSAGE_ADD_SESSION_IP,
		session_name:session_name,
		ip:ip
	};

	for(let worker of workers){
		worker.worker.postMessage(message);
	}
}

function send_chunks_to_workers(){
	let chunk_lists:{[index:number]:ChunkListEntryToWorker[]} = {};
	//let transfer_lists = {};
	for (const session of Object.values(sessions)){
		switch(session.state){
			case SessionMode.SESSION_MODE_PDP:
			case SessionMode.SESSION_MODE_PTP_ACCEPT:
			case SessionMode.SESSION_MODE_PTP_CONNECT:{
				if (session.worker == undefined){
					break;
				}
				const worker_id = session.worker.id;
				let chunk_list = chunk_lists[worker_id];
				if (chunk_list == undefined){
					chunk_list = [];
					chunk_lists[worker_id] = chunk_list;
				}
				/*
				let transfer_list = transfer_lists[worker_id];
				if (transfer_list == undefined){
					transfer_list = [];
					transfer_lists[worker_id] = transfer_list;
				}
				*/
				chunk_list.push({
					session_name:session.session_name,
					chunks:session.chunks
				});
				/*
				for(const chunk of session.chunks){
					transfer_list.push(chunk.buffer);
				}
				*/
				session.chunks = [];
				break;
			}
			case SessionMode.SESSION_MODE_PTP_LISTEN:
				break;
			default:
				log(`bad session state ${session.state} while sending chunks to workers, debug this`);
				process.exit(1);
		}
	}

	for (const [id, chunk_list] of Object.entries(chunk_lists)){
		if (chunk_list.length == 0){
			continue;
		}
		workers[Number(id)].worker.postMessage({
			type:ParentToWorkerMessageType.PARENT_MESSAGE_HANDLE_CHUNK,
			chunk_list:chunk_list,
		});
	}
}

function create_session(ctx:Session){
	let type = ctx.init_data.subarray(0, 4).readInt32LE();
	let src_addr = ctx.init_data.subarray(4, 12);
	let sport = ctx.init_data.subarray(12, 14).readUInt16LE();
	let dst_addr = ctx.init_data.subarray(14, 22);
	let dport = ctx.init_data.subarray(22, 24).readUInt16LE();

	ctx.src_addr = src_addr;
	ctx.sport = sport;
	ctx.dst_addr = dst_addr;
	ctx.dport = dport;

	ctx.src_addr_str = get_mac_str(ctx.src_addr);
	ctx.dst_addr_str = get_mac_str(ctx.dst_addr);

	clearTimeout(ctx.init_timeout);

	if (!strict_mode_verify_ip_addr(ctx.src_addr_str, ctx.ip)){
		ctx.socket.destroy();
		return;
	}

	const num_ips = Object.keys(sessions_by_ip).length;
	const sessions_of_this_ip = sessions_by_ip[ctx.ip];
	const max_ips = config.max_ips;
	if (max_ips != 0 && sessions_of_this_ip == undefined && num_ips >= max_ips){
		ctx.socket.destroy();
		return;
	}

	switch(type){
		case InitPacketType.AEMU_POSTOFFICE_INIT_PDP:{
			ctx.state = SessionMode.SESSION_MODE_PDP;
			ctx.session_name = `PDP ${get_mac_str(src_addr)} ${sport}`;
			ctx.pdp_data = ctx.outstanding_data;
			ctx.pdp_state = PdpState.PDP_STATE_HEADER;
			remove_existing_and_insert_session(ctx, ctx.session_name);
			log(`created session ${ctx.session_name} for ${ctx.sock_addr_str}`);
			if (config.accounting_interval_ms > 0){
				track_connect(ctx.ip, false, false);
			}

			add_session_to_worker(ctx);
			add_session_ip_to_workers(ctx.session_name, ctx.ip);

			ctx.init_data = Buffer.allocUnsafe(0);
			ctx.outstanding_data = Buffer.allocUnsafe(0);

			break;
		}
		case InitPacketType.AEMU_POSTOFFICE_INIT_PTP_LISTEN:{
			ctx.state = SessionMode.SESSION_MODE_PTP_LISTEN;
			ctx.session_name = `PTP_LISTEN ${get_mac_str(src_addr)} ${sport}`;
			remove_existing_and_insert_session(ctx, ctx.session_name);
			log(`created session ${ctx.session_name} for ${ctx.sock_addr_str}`);
			if (config.accounting_interval_ms > 0){
				track_connect(ctx.ip, true, true);
			}

			ctx.init_data = Buffer.allocUnsafe(0);
			ctx.outstanding_data = Buffer.allocUnsafe(0);

			break;
		}
		case InitPacketType.AEMU_POSTOFFICE_INIT_PTP_CONNECT:{
			ctx.session_name = `PTP_CONNECT ${get_mac_str(src_addr)} ${sport} ${get_mac_str(dst_addr)} ${dport}`;

			let listen_session = find_target_session(SessionMode.SESSION_MODE_PTP_LISTEN, ctx.src_addr_str, ctx.dst_addr_str, 0, ctx.dport);
			if (listen_session == undefined){
				// 2 seconds of retries
				if (ctx.ptp_connect_retries < 8){
					const retry = () => {
						if (ctx.state != SessionMode.SESSION_MODE_INIT){
							return;
						}
						create_session(ctx);
					}
					ctx.ptp_connect_retries++;
					setTimeout(retry, 250);
					break;
				}
				const target_session_name = get_target_session_name(SessionMode.SESSION_MODE_PTP_LISTEN, ctx.src_addr_str, ctx.dst_addr_str, 0, ctx.dport);
				log(`not creating ${ctx.session_name} for ${ctx.sock_addr_str}, ${target_session_name} not found`);
				ctx.socket.destroy();
				break;
			}

			ctx.state = SessionMode.SESSION_MODE_PTP_CONNECT;

			remove_existing_and_insert_session(ctx, ctx.session_name);
			let port = Buffer.allocUnsafe(2);
			port.writeUInt16LE(sport);
			ctx.ptp_state = PtpState.PTP_STATE_WAITING;
			ctx.ptp_data = ctx.outstanding_data;
			listen_session.socket.write(Buffer.concat([src_addr, port]));
			const max_buffer_size = config.max_write_buffer_byte;
			if (max_buffer_size != 0 && listen_session.socket.writableLength >= max_buffer_size){
				log(`killing session ${listen_session.session_name} as write buffer has reached ${listen_session.socket.writableLength} bytes, max ${max_buffer_size} bytes`);
				close_session(listen_session);
				log(`not creating ${ctx.session_name} for ${ctx.sock_addr_str}, ${listen_session.session_name} is stale`);
				ctx.socket.destroy();
				break;
			}

			log(`created session ${ctx.session_name} for ${ctx.sock_addr_str}`);
			if (config.accounting_interval_ms > 0){
				track_connect(ctx.ip, true, false);
			}

			ctx.ptp_wait_timeout = setTimeout(() => {
				if (ctx.ptp_state == PtpState.PTP_STATE_WAITING){
					log(`the other side did not accept the connection request in 20 seconds, killing ${ctx.session_name} of ${ctx.sock_addr_str}`);
					close_session(ctx);
				}
			}, 20000);

			break;
		}
		case InitPacketType.AEMU_POSTOFFICE_INIT_PTP_ACCEPT:{
			ctx.state = SessionMode.SESSION_MODE_PTP_ACCEPT;
			ctx.session_name = `PTP_ACCEPT ${get_mac_str(src_addr)} ${sport} ${get_mac_str(dst_addr)} ${dport}`

			let connect_session = find_target_session(SessionMode.SESSION_MODE_PTP_CONNECT, ctx.src_addr_str, ctx.dst_addr_str, ctx.sport, ctx.dport);
			if (connect_session == undefined || connect_session.ptp_state != PtpState.PTP_STATE_WAITING){
				const target_session_name = get_target_session_name(SessionMode.SESSION_MODE_PTP_CONNECT, ctx.src_addr_str, ctx.dst_addr_str, ctx.sport, ctx.dport);
				log(`${target_session_name} not found, closing ${ctx.session_name} of ${ctx.sock_addr_str}`);
				ctx.socket.destroy();
				break;
			}

			remove_existing_and_insert_session(ctx, ctx.session_name);
			ctx.peer_session = connect_session;
			connect_session.peer_session = ctx;
			ctx.ptp_state = PtpState.PTP_STATE_HEADER;
			connect_session.ptp_state = PtpState.PTP_STATE_HEADER;
			clearTimeout(connect_session.ptp_wait_timeout);
			ctx.ptp_data = ctx.outstanding_data;
			ctx.peer_session_name = ctx.peer_session.session_name;
			connect_session.peer_session_name = connect_session.peer_session.session_name;

			let port = Buffer.allocUnsafe(2);
			port.writeUInt16LE(sport);
			connect_session.socket.write(Buffer.concat([ctx.src_addr, port]));
			port.writeUInt16LE(dport);
			ctx.socket.write(Buffer.concat([ctx.dst_addr, port]));
			log(`created session ${ctx.session_name} for ${ctx.sock_addr_str}`);
			if (config.accounting_interval_ms > 0){
				track_connect(ctx.ip, true, false);
			}

			add_session_to_worker(connect_session);
			add_session_to_worker(ctx);

			add_session_ip_to_workers(connect_session.session_name, ctx.ip);
			add_session_ip_to_workers(connect_session.session_name, ctx.ip);

			connect_session.init_data = Buffer.allocUnsafe(0);
			ctx.init_data = Buffer.allocUnsafe(0);

			connect_session.outstanding_data = Buffer.allocUnsafe(0);
			ctx.outstanding_data = Buffer.allocUnsafe(0);

			break;
		}
		default:
			log(`${ctx.sock_addr_str} has bad init type ${type}, dropping connection`);
			ctx.socket.destroy();
	}
}

function on_connection(socket:net.Socket){
	socket.setKeepAlive(true);
	socket.setNoDelay(true);

	let ctx:Session = {
		src_addr:Buffer.allocUnsafe(0),
		sport:0,
		dst_addr:Buffer.allocUnsafe(0),
		dport:0,
		src_addr_str:"",
		dst_addr_str:"",
		state:SessionMode.SESSION_MODE_INIT,
		session_name:"",
		pdp_data:Buffer.allocUnsafe(0),
		ptp_data:Buffer.allocUnsafe(0),
		peer_session_name:"",
		pdp_state:PdpState.PDP_STATE_HEADER,
		ptp_state:PtpState.PTP_STATE_WAITING,
		sock_addr_str:get_sock_addr_str(socket),
		socket:socket,
		worker:undefined,
		chunks:[],
		peer_session:undefined,
		init_data:Buffer.allocUnsafe(0),
		outstanding_data:Buffer.allocUnsafe(0),
		outstanding_data_created:false,
		ip:socket.remoteAddress == undefined ? "" : socket.remoteAddress,
		ptp_wait_timeout:0,
		init_timeout:0,
		ptp_connect_retries:0,
	};

	socket.on("error", (err) => {
		switch(ctx.state){
			case SessionMode.SESSION_MODE_INIT:
				log(`${ctx.sock_addr_str} errored during init, ${err}`);
				ctx.socket.destroy();
				break;
			case SessionMode.SESSION_MODE_PDP:
			case SessionMode.SESSION_MODE_PTP_LISTEN:
				log(`${ctx.session_name} ${ctx.sock_addr_str} errored, ${err}`);
				close_session(ctx);
				break;
			case SessionMode.SESSION_MODE_PTP_CONNECT:
			case SessionMode.SESSION_MODE_PTP_ACCEPT:
				log(`${ctx.session_name} ${ctx.sock_addr_str} errored, ${err}`);
				close_session(ctx);
				break;
			default:
				log(`bad state ${ctx.state} on socket error, debug this`);
				process.exit(1);
		}
	})

	socket.on("end", () => {
		switch(ctx.state){
			case SessionMode.SESSION_MODE_INIT:
				log(`${ctx.sock_addr_str} closed during init`);
				ctx.socket.destroy();
				break;
			case SessionMode.SESSION_MODE_PDP:
			case SessionMode.SESSION_MODE_PTP_LISTEN:
				log(`${ctx.session_name} ${ctx.sock_addr_str} closed by client`);
				close_session(ctx);
				break;
			case SessionMode.SESSION_MODE_PTP_CONNECT:
			case SessionMode.SESSION_MODE_PTP_ACCEPT:
				log(`${ctx.session_name} ${ctx.sock_addr_str} closed by client`);
				setTimeout(()=>{close_session(ctx)}, (1000 / config.tick_rate_hz) * 10);
				break;
			default:
				log(`bad state ${ctx.state} on socket end, debug this`);
				process.exit(1);
		}
	})

	socket.on("data", (new_data:Buffer) => {
		switch(ctx.state){
			case SessionMode.SESSION_MODE_INIT:{
				if (!ctx.outstanding_data_created){
					ctx.init_data = Buffer.concat([ctx.init_data, new_data]);
					if (ctx.init_data.length >= 24){
						ctx.outstanding_data = ctx.init_data.subarray(24);
						ctx.outstanding_data_created = true;
						ctx.init_data = ctx.init_data.subarray(0, 24);
						create_session(ctx);
					}
				}else{
					ctx.outstanding_data = Buffer.concat([ctx.outstanding_data, new_data]);
				}

				break;
			}
			case SessionMode.SESSION_MODE_PDP:{
				ctx.chunks.push(new_data);
				break;
			}
			case SessionMode.SESSION_MODE_PTP_LISTEN:{
				// we just discard incoming data for ptp_listen
				break;
			}
			case SessionMode.SESSION_MODE_PTP_CONNECT:
			case SessionMode.SESSION_MODE_PTP_ACCEPT:{
				ctx.chunks.push(new_data);
				break;
			}
			default:{
				log(`bad state ${ctx.state} on socket data handler, debug this`);
				process.exit(1);
			}
		}
	});

	ctx.init_timeout = setTimeout(() => {
		if (ctx.state == SessionMode.SESSION_MODE_INIT){
			log(`removing stale connection ${ctx.sock_addr_str}`);
			ctx.socket.destroy();
		}
	}, 20000)
}

if (worker_threads.isMainThread){
	let server = net.createServer();

	server.maxConnections = config.max_connections;

	server.on("error", (err) => {
		throw err;
	});

	server.on("drop", (drop) => {
		log(`connection dropped as we have reached ${server.maxConnections} connections:`);
		log(drop);
	});

	for (let i = 0;i < config.num_worker_threads;i++){
		let worker = {
			id:i,
			num_sessions:0,
			worker:new worker_threads.Worker(__filename),
		};
		worker.worker.on("message", handle_worker_message);
		worker.worker.once("error", (e) => {
			log(`worker error `, e, ` debug this`);
			process.exit(1);
		});
		workers.push(worker);
	}

	process.on('SIGHUP', () => {
		if (os.platform() == 'win32'){
			process.exit(0);
		}
		log(`reloading config on SIGHUP`);
		load_config();
		for (const worker of workers){
			worker.worker.postMessage({
				type:ParentToWorkerMessageType.PARENT_MESSAGE_UPDATE_CONFIG,
				config:config,
			});
		}
	});

	server.on("connection", on_connection);

	run_per_tick(send_chunks_to_workers);

	log(`begin listening on port ${port}`);

	server.listen({
		port:port,
		backlog:1000
	});
}else{
	if (worker_threads.parentPort != undefined){
		worker_threads.parentPort.on("message", handle_parent_message);
	}

	run_per_tick(send_data_to_parent);
}

function send_adhocctl_data_to_workers(){
	const message = {
		type:ParentToWorkerMessageType.PARENT_MESSAGE_SYNC_ADHOCCTL_DATA,
		adhocctl_groups_by_mac:adhocctl_groups_by_mac,
	};

	for(let worker of workers){
		worker.worker.postMessage(message);
	}
}

function game_list_sync(request:http.IncomingMessage, response:http.ServerResponse){
	let ctx = {buf:Buffer.allocUnsafe(0)};
	request.on("data", (chunk) => {
		ctx.buf = Buffer.concat([ctx.buf, chunk]);
	});
	request.on("end", () => {
		const decoded_string = ctx.buf.toString("utf8");
		let parsed_data:any = {};
		try{
			parsed_data = JSON.parse(decoded_string)
		}catch(e){
			log(`failed parsing game list update from ${request.socket.remoteAddress}`);
			response.writeHead(400);
			response.end("bad data");
			return;
		}

		const games:any = parsed_data["games"];
		if (games == undefined){
			log(`incoming game list has no game array..`);
			response.writeHead(400);
			response.end("bad data");
			return;
		}

		let processed_data:AdhocctlData = {
			games:[]
		};

		let processed_groups_by_mac:{[index:string]:AdhocctlGroup} = {};
		let processed_players_by_mac:{[index:string]:AdhocctlPlayer} = {};

		for (const game of games){
			const groups = game["groups"];
			if (groups == undefined){
				continue;
			}
			let processed_game:AdhocctlGame = {
				groups:[]
			};
			processed_data.games.push(processed_game);
			for (const group of groups){
				const players = group["players"];
				if (players == undefined){
					continue;
				}
				let processed_group:AdhocctlGroup = {};
				processed_game.groups.push(processed_group);
				for (const player of players){
					let processed_player:AdhocctlPlayer = {
						mac_addr:player["mac_addr"].toLowerCase(),
						ip_addr:player["ip_addr"],
					}
					processed_groups_by_mac[processed_player.mac_addr] = processed_group;
					processed_players_by_mac[processed_player.mac_addr] = processed_player;
					processed_group[processed_player.mac_addr] = processed_player;
				}
			}
		}
		adhocctl_data = processed_data;
		adhocctl_groups_by_mac = processed_groups_by_mac;
		adhocctl_players_by_mac = processed_players_by_mac;
		send_adhocctl_data_to_workers();
		response.writeHead(200);
		response.end("data accepted");
	});
}

function data_debug(request:http.IncomingMessage, response:http.ServerResponse){
	let response_obj:any = {
		adhocctl_data:adhocctl_data,
		adhocctl_groups_by_mac:adhocctl_groups_by_mac,
		adhocctl_players_by_mac:adhocctl_players_by_mac,
	};

	let convert_session_list = (from_list:AddrSessionsMap) => {
		let to_list:any = {}
		for (const [key, sessions] of Object.entries(from_list)){
			let response_sessions:any = [];
			to_list[key] = response_sessions;
			for(const session of Object.values(sessions)){
				let response_session:any = {
					session_name:session.session_name,
					ip:session.ip,
					write_buffer_size:session.socket.writableLength,
				};
				response_sessions.push(response_session);

				switch(session.state){
					case SessionMode.SESSION_MODE_PDP:
						response_session.pdp_state = session.pdp_state;
						break;
					case SessionMode.SESSION_MODE_PTP_LISTEN:
						break;
					case SessionMode.SESSION_MODE_PTP_CONNECT:
					case SessionMode.SESSION_MODE_PTP_ACCEPT:
						response_session.ptp_state = session.ptp_state;
						response_session.dst_addr = session.dst_addr_str;
						response_session.dport = session.dport;
						break;
					default:
						log(`bad state ${session.state} on data debug, debug this`);
						process.exit(1);
				}
			}
		}
		return to_list;
	};

	response_obj["sessions_by_mac"] = convert_session_list(sessions_by_mac);
	response_obj["sessions_by_ip"] = convert_session_list(sessions_by_ip);
	response_obj["memory_usage"] = process.memoryUsage();

	response.writeHead(200, {"Content-Type": "application/json", "Access-Control-Allow-Origin": "*"});
	response.end(JSON.stringify(response_obj));
}

interface Routes{
	[index:string]:(request:http.IncomingMessage, response:http.ServerResponse) => void,
}

const routes:Routes = {
	"/game_list_sync":game_list_sync,
	"/data_debug":data_debug,
};

function session_mode_to_string(mode:SessionMode){
	switch(mode){
		case SessionMode.SESSION_MODE_INIT:
			return "init";
		case SessionMode.SESSION_MODE_PDP:
			return "pdp";
		case SessionMode.SESSION_MODE_PTP_LISTEN:
			return "ptp_listen";
		case SessionMode.SESSION_MODE_PTP_CONNECT:
			return "ptp_connect";
		case SessionMode.SESSION_MODE_PTP_ACCEPT:
			return "ptp_accept";
		default:
			log(`bad mode ${mode} for string conversion, debug this`);
			process.exit(1);
	}
}

interface SessionListEntry{
	state:string,
	src_addr:string,
	sport:number,
	pdp_state?:PdpState,
	ptp_state?:PtpState,
	dst_addr?:string,
	dport?:number,
}

if (worker_threads.isMainThread){
	let status_server = http.createServer();
	status_server.on("error", (err) => {
		throw err;
	});

	status_server.on("request", (request:http.IncomingMessage, response:http.ServerResponse) => {
		let ret:{[index:string]:SessionListEntry[]} = {};
		const url = request.url == undefined ? "" : request.url;
		const route:(request:http.IncomingMessage, response:http.ServerResponse) => void | undefined = routes[url];
		if (route != undefined){
			route(request, response);
			return;
		}
		for (let entry of Object.entries(sessions)){
			let ctx = entry[1];
			let ret_entry:SessionListEntry = {
				state:session_mode_to_string(ctx.state),
				src_addr:ctx.src_addr_str,
				sport:ctx.sport
			};

			switch(ctx.state){
				case SessionMode.SESSION_MODE_PDP:
					ret_entry.pdp_state = ctx.pdp_state;
					break;
				case SessionMode.SESSION_MODE_PTP_LISTEN:
					break;
				case SessionMode.SESSION_MODE_PTP_CONNECT:
				case SessionMode.SESSION_MODE_PTP_ACCEPT:
					ret_entry.ptp_state = ctx.ptp_state;
					ret_entry.dst_addr = ctx.dst_addr_str;
					ret_entry.dport = ctx.dport;
					break;
				default:
					log(`bad state ${ctx.state} on status query, debug this`);
					process.exit(1);
			}

			if (ret[entry[1].src_addr_str] == undefined){
				ret[entry[1].src_addr_str] = [];
			}
			ret[entry[1].src_addr_str].push(ret_entry);
		}

		response.writeHead(200, {"Content-Type": "application/json", "Access-Control-Allow-Origin": "*"});
		response.end(JSON.stringify(ret));
	});

	log(`begin listening on port ${status_port} for server status`)

	status_server.listen({
		port:status_port,
		backlog:1000
	});
}
