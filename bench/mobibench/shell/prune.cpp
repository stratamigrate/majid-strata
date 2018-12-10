#include<cstdio>
#include<iostream>
#include<string.h>
#include <fstream>
#include <sstream>

using namespace std;

int main(){
   //freopen("./majid.facebook", "rb", stdin);
   //string line
   //;
   //
   //
   ifstream infile;

   int num;
   infile.open("./majid.facebook");


   std::string line;
   while (std::getline(infile, line))
   {
     std::stringstream ss(line);
     string a, b, c;
     if (ss >> a >> b >> c)
     {
	     if(c=="read" || c=="write")
//          	     cout<<a<<" -- "<<b<<" -- "<<c<<endl;
          	     cout<<line<<endl;
         // Add a, b, and c to their respective arrays
     }
  }
	   
	   
	   
/*
   while(getline(cin, line)){
	   string f;
	   cin >> f;
	   cin>>f;
	   cin>>f;
	   if(f=="read" || f=="write"){
		   cout<<line<<endl;
	   }
	   //cout<<"first "<<f<<endl;
	   //cin>>f;
	   ///cout<<"second "<<f<<endl;
	   
   }
       cout << line << endl;*/


   return 0;
}
