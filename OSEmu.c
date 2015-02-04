#include "globals.h"
#include "helpfunctions.h"
#include "emulator.h"

static struct aes_keys cl_aes_keys;
static uchar cl_ucrc[4];
static unsigned char cl_user[128];
static unsigned char cl_passwd[128];

static int cl_sockfd;
static struct sockaddr_in cl_socket;

int8_t debuglog = 0;
int8_t havelogfile = 0;
int8_t requestau = 0;
char*  logfile = NULL;
int bg = 0;

#define REQ_SIZE	584		// 512 + 20 + 0x34

#define suppresscmd08 1

static int32_t do_daemon(int32_t nochdir, int32_t noclose)
{
	int32_t fd = 0;

	switch (fork())
	{
		case -1: return (-1);
		case 0:  break;
		default: _exit(0);
	}

	if (setsid() == (-1))
	return (-1);

	if (!nochdir)
	(void)chdir("/");

	if (!noclose && (fd = open("/dev/null", O_RDWR, 0)) != -1)
	{
		(void)dup2(fd, STDIN_FILENO);
		(void)dup2(fd, STDOUT_FILENO);
		(void)dup2(fd, STDERR_FILENO);
	if (fd > 2)
		(void)close(fd);
	}
	return (0);
}

static int32_t camd35_send(uchar *buf, int32_t buflen)
{
	int32_t l, status;
	unsigned char rbuf[REQ_SIZE + 15 + 4], *sbuf = rbuf + 4;

	//Fix ECM len > 255
	if(buflen <= 0)
		{ buflen = ((buf[0] == 0) ? (((buf[21] & 0x0f) << 8) | buf[22]) + 3 : buf[1]); }
	l = 20 + (((buf[0] == 3) || (buf[0] == 4)) ? 0x34 : 0) + buflen;
	memcpy(rbuf, cl_ucrc, 4);
	memcpy(sbuf, buf, l);
	memset(sbuf + l, 0xff, 15); // set unused space to 0xff for newer camd3's
	i2b_buf(4, crc32(0, sbuf + 20, buflen), sbuf + 4);
	l = boundary(4, l);
	cs_log_debug("send %d bytes to client", l);
	aes_encrypt_idx(&cl_aes_keys, sbuf, l);

	status = sendto(cl_sockfd, rbuf, l + 4, 0, (struct sockaddr *)&cl_socket, sizeof(cl_socket));
	return status;
}

static int32_t camd35_auth_client(uchar *ucrc)
{
	int32_t rc = 1;
	uint32_t crc;
	unsigned char md5tmp[MD5_DIGEST_LENGTH];
	crc = (((ucrc[0] << 24) | (ucrc[1] << 16) | (ucrc[2] << 8) | ucrc[3]) & 0xffffffff);

	if(crc == crc32(0, MD5(cl_user, strlen((char *)cl_user), md5tmp), MD5_DIGEST_LENGTH))
	{
		memcpy(cl_ucrc, ucrc, 4);
		return 0;
	}
	return (rc);
}

static int32_t camd35_recv(uchar *buf, int32_t rs)
{
	int32_t rc, s, n = 0, buflen = 0;
	for(rc = s = 0; !rc; s++)
	{
		switch(s)
		{
		case 0:
			if(rs < 36)
			{
				rc = -1;
				goto out;
			}
			break;
		case 1:
			switch(camd35_auth_client(buf))
			{
			case  0:
				break;  // ok
			case  1:
				rc = -2;
				break; // unknown user
			default:
				rc = -9;
				break; // error's from cs_auth()
			}
			memmove(buf, buf + 4, rs -= 4);
			break;
		case 2:
			aes_decrypt(&cl_aes_keys, buf, rs);
			if(rs != boundary(4, rs))
				cs_log_debug("WARNING: packet size has wrong decryption boundary");

			n = (buf[0] == 3) ? 0x34 : 0;

			//Fix for ECM request size > 255 (use ecm length field)
			if(buf[0] == 0)
				{ buflen = (((buf[21] & 0x0f) << 8) | buf[22]) + 3; }
			else if(buf[0] == 0x3d || buf[0] == 0x3e || buf[0] == 0x3f)  //cacheex-push
				{ buflen = buf[1] | (buf[2] << 8); }
			else
				{ buflen = buf[1]; }

			n = boundary(4, n + 20 + buflen);

			cs_log_debug("received %d bytes from client", rs);

			if(n < rs)
				cs_log_debug("ignoring %d bytes of garbage", rs - n);
			else if(n > rs) { rc = -3; }
			break;
		case 3:
			if(crc32(0, buf + 20, buflen) != b2i(4, buf + 4)) { 
				rc = -4;
				cs_log_hexdump("camd35 checksum failed for packet: ", buf, rs);
				cs_log_debug("checksum: %X", b2i(4, buf+4)); 
			}
			if(!rc) { rc = n; }
			break;
		}
	}

out:
	if((rs > 0) && ((rc == -1) || (rc == -2)))
	{
		cs_log_debug("received %d bytes from client (native)", rs);
	}
	switch(rc)
	{
	case -1:
		cs_log("packet is too small (received %d bytes, expected at least 36 bytes)", rs);
		break;
	case -2:
		cs_log("unknown user");
		break;
	case -3:
		cs_log("incomplete request !");
		break;
	case -4:
		cs_log("checksum error (wrong password ?)");
		break;
	}

	return (rc);
}

static void camd35_request_emm(uint16_t caid, uint32_t provider, uint8_t* hexserial,
								uint8_t emm_global, uint8_t emm_shared, uint8_t emm_unique)
{
	uchar mbuf[1024];
	uint8_t prid[4];

	memset(mbuf, 0, sizeof(mbuf));
	mbuf[2] = mbuf[3] = 0xff;           // must not be zero
	
	i2b_buf(4, provider, &mbuf[12]);

	mbuf[0] = 5;
	mbuf[1] = 111;
	if(caid)
	{
		mbuf[39] = 1;                // no. caids
		mbuf[20] = caid >> 8;        // caid's (max 8)
		mbuf[21] = caid & 0xff;

		memcpy(mbuf + 40, hexserial, 6); 
		mbuf[47] = 1;

		prid[0] = provider >> 24;
		prid[1] = provider >> 16;
		prid[2] = provider >> 8;
		prid[3] = provider & 0xFF;	

		if((caid >= 0x1700 && caid <= 0x1799)  ||  // Betacrypt
				(caid >= 0x0600 && caid <= 0x0699))    // Irdeto (don't know if this is correct, cause I don't own a IRDETO-Card)
		{
			mbuf[48 + (0 * 5)] = prid[0];
			memcpy(&mbuf[50 + (0 * 5)], &prid[1], 3);
		}
		else
		{
			mbuf[48 + (0 * 5)] = prid[2];
			mbuf[49 + (0 * 5)] = prid[3];
		}

		mbuf[127] = 0; //EMM_UNKNOWN
		mbuf[128] = emm_global; //EMM_GLOBAL
		mbuf[129] = emm_shared; //EMM_SHARED
		mbuf[130] = emm_unique; //EMM_UNIQUE
	}
	else        // disable emm
		{ mbuf[20] = mbuf[39] = mbuf[40] = mbuf[47] = mbuf[49] = 1; }

	memcpy(mbuf + 10, mbuf + 20, 2);
	camd35_send(mbuf, 0);       // send with data-len 111 for camd3 > 3.890
	mbuf[1]++;
	camd35_send(mbuf, 0);       // send with data-len 112 for camd3 < 3.890
}

static void camd35_process_ecm(uchar *buf, int buflen)
{
	ECM_REQUEST er;
	uint16_t ecmlen = 0;
	uint8_t hexserial[6];
	
	if(!buf || buflen < 23)
		{ return; }
	
	ecmlen = (((buf[21] & 0x0f) << 8) | buf[22]) + 3;
	if(ecmlen + 20 > buflen)
		{ return; }
	
	memset(&er, 0, sizeof(ECM_REQUEST));
    er.rc = E_UNHANDLED;
	er.ecmlen = ecmlen;
	er.srvid = b2i(2, buf + 8);
	er.caid = b2i(2, buf + 10);
	er.prid = b2i(4, buf + 12);
	
	cs_log_debug("ProcessECM CAID: %X", er.caid);
	cs_log_hexdump("ProcessECM: ", buf+20, ecmlen);
	
	if(ProcessECM(er.ecmlen,er.caid,er.prid,buf+20,er.cw,er.srvid,er.pid)) {
	  er.rc = E_NOTFOUND;
	  cs_log_debug("CW not found");
	}
	else {
	  er.rc = E_FOUND;
	  cs_log_hexdump("Found CW: ", er.cw, 16);
    }
    
	if((er.rc == E_NOTFOUND || (er.rc == E_INVALID)) && !suppresscmd08)
	{
		buf[0] = 0x08;
		buf[1] = 2;
		memset(buf + 20, 0, buf[1]);
		buf[22] = er.rc; //put rc in byte 22 - hopefully don't break legacy camd3
	}
	else if(er.rc == E_STOPPED)
	{
		buf[0] = 0x08;
		buf[1] = 2;
		buf[20] = 0;
		/*
		 * the second Databyte should be forseen for a sleeptime in minutes
		 * whoever knows the camd3 protocol related to CMD08 - please help!
		 * on tests this don't work with native camd3
		 */
		buf[21] = 0;
		cs_log("client stop request send");
	}
	else
	{
		// Send CW
		if((er.rc < E_NOTFOUND) || (er.rc == E_FAKE))
		{
			if(buf[0] == 3)
				{ memmove(buf + 20 + 16, buf + 20 + buf[1], 0x34); }
			buf[0]++;
			buf[1] = 16;
			memcpy(buf + 20, er.cw, buf[1]);
		}
		else
		{
			// Send old CMD44 to prevent cascading problems with older mpcs/oscam versions
			buf[0] = 0x44;
			buf[1] = 0;
		}
	}
	camd35_send(buf, 0);
	
	if(requestau) {
		if(er.caid == 0x0500) {
			memset(hexserial, 0, 6);
			camd35_request_emm(er.caid, 0x030B00, hexserial, 1, 0, 0);	
		}
		else if(er.caid == 0x0604) {
			memset(hexserial, 0, 6);
			GetIrdeto2Hexserial(er.caid, hexserial);
			camd35_request_emm(er.caid, 0x010200, hexserial, 1, 1, 1);
		}
	}
}

static void camd35_process_emm(uchar *buf, int buflen)
{
	uint32_t keysAdded = 0;
	uint16_t emmlen = 0;
	
	if(!buf || buflen < 23)
		{ return; }
		
	emmlen = (((buf[21] & 0x0f) << 8) | buf[22]) + 3;
	if(emmlen + 20 > buflen)
		{ return; }
		
	cs_log_debug("ProcessEMM CAID: %X", (buf[10] << 8) | buf[11]);
	cs_log_hexdump("ProcessEMM: ", buf+20, emmlen);
	
	if(ProcessEMM((buf[10] << 8) | buf[11],
      (buf[12] << 24) | (buf[13] << 16) | (buf[14] << 8) | buf[15],buf+20,&keysAdded)) {
	  cs_log_debug("EMM nok");
	}
	else {
	  cs_log_debug("EMM ok");
    }	
}

void show_usage(char *cmdline){
	cs_log("Usage: %s -a <user>:<password> -p <port> [-b -v -e -c <path> -l <logfile>]", cmdline);
	cs_log("-b enables to start as a daemon (background)");
	cs_log("-v enables a more verbose output (debug output)");
	cs_log("-e enables emm au");
	cs_log("-c sets path of SoftCam.Key");
	cs_log("-l sets log file");
}

int main(int argc, char**argv)
{
	int n, opt, port = 0, accountok = 0;
	struct sockaddr_in servaddr;
	socklen_t len;
	unsigned char mbuf[20+1024];
	unsigned char md5tmp[MD5_DIGEST_LENGTH];
	char *path = "./";

	cs_log("OSEmu version %d", GetOSemuVersion());
	
	while ((opt = getopt(argc, argv, "bva:p:c:l:e")) != -1) {
		switch (opt) {
			case 'b':
				bg = 1;
				break;
			case 'a': {
				char *ptr = strtok(optarg, ":");
				cs_strncpy((char *)&cl_user, ptr, sizeof(cl_user));
				ptr = strtok(NULL, ":");
				if(ptr) {
					cs_strncpy((char *)&cl_passwd, ptr, sizeof(cl_passwd));
					accountok = 1;
				}
				break;
			}
			case 'p':
				port = atoi(optarg);
				break;
			case 'v':
				debuglog = 1;
				break;
			case 'c':
				path = strdup(optarg);
				break;
			case 'l':
				logfile = strdup(optarg);
				havelogfile = 1;
				break;
			case 'e':
				requestau = 1;
				break;	
			default:
				show_usage(argv[0]);
				exit(0);
		}
	}
	if(port == 0 || accountok == 0){
		show_usage(argv[0]);
		exit(0);
	}

	if (bg && do_daemon(1, 0))
	{
		cs_log("Could not start as a daemon.");	
		exit(0);
	}
	
	get_random_bytes_init();

#if !defined(__APPLE__) && !defined(__ANDROID__)
	read_emu_keymemory();
#endif

	if(!read_emu_keyfile(path)) {
		read_emu_keyfile("/var/keys/");
	}
	
	cl_sockfd = socket(AF_INET,SOCK_DGRAM,0);
	if(cl_sockfd == -1) {
		cs_log("Could not create socket.");	
		exit(0);		
	}
		
	bzero(&servaddr,sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = htons(port);
	if(bind(cl_sockfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) == -1) {
		cs_log("Could not bind to socket.");	
		exit(0);			
	}
	
	aes_set_key(&cl_aes_keys, (char *) MD5(cl_passwd, strlen((char *)cl_passwd), md5tmp));
	
	for (;;){
		len = sizeof(cl_socket);
		n = recvfrom(cl_sockfd,mbuf,sizeof(mbuf),0,(struct sockaddr *)&cl_socket,&len);
		
		if (camd35_recv(mbuf, n) >= 0 ){
			if(mbuf[0] == 0 || mbuf[0] == 3) {
				camd35_process_ecm(mbuf, n);
			}
			else if((mbuf[0] == 6 || mbuf[0] == 19)) {
				camd35_process_emm(mbuf, n);
			} 
			else {
				cs_log("unknown/not implemented camd35 command! (%d) n=%d", mbuf[0], n);
			}
		}
	}
}
